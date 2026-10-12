// Deterministic ZIP (.pak) writer and content-addressed payload cache for NOX packages.
// Contract C5 (docs/packaging.md). Compiled by tools/AssetPackaging.ps1 through Add-Type, which uses
// the .NET Framework C# 5 compiler on Windows PowerShell 5.1: keep this file to C# 5 syntax.
//
// Archive rules: entries sorted by ordinal name; fixed DOS timestamp 1980-01-01 00:00; no extra
// fields, no data descriptors, no ZIP64, ASCII names; Deflate only when it saves
// max(64 B, 1%), otherwise stored. Writes a temp file, re-reads and verifies every entry, then
// replaces the target. An unchanged index (names, methods, CRCs, sizes, offsets) skips the write.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;

public sealed class NoxPackInput
{
    public string Name;   // logical path: ASCII, '/' separated, no '..', no leading '/'
    public string Path;   // source file (used when Bytes is null)
    public byte[] Bytes;  // generated content (takes precedence over Path)
    public bool ForceStored; // seek-heavy or already-compressed format: never deflate (packaging/pack_rules.json)
}

public sealed class NoxPackEntry
{
    public string Name;
    public int Method;             // 0 stored, 8 deflate
    public uint Crc;
    public long Size;              // uncompressed
    public long CompressedSize;
    public string Sha256;          // lowercase hex of the uncompressed bytes
    public string DataPath;        // file holding the entry payload (cache payload or source file)
    public byte[] DataBytes;       // in-memory stored payload when there is no file
    public long Offset;            // local header offset in the archive (set by Build)
    public bool CacheHit;
    public bool Recovered;         // a corrupt cache entry was discarded and rebuilt
}

public sealed class NoxPackResult
{
    public bool Rewritten;
    public string Path;
    public long FileBytes;
    public string IndexSha256;     // sha256 of the planned pack index
    public int CacheHits;
    public int Cooked;
    public int Recovered;
    public List<NoxPackEntry> Entries = new List<NoxPackEntry>();
}

public static class NoxPack
{
    public const string PolicyVersion = "adaptive-deflate-optimal-v1";
    public const string ToolVersion = "nox-pack-writer-1";
    public const int MinSavingBytes = 64;
    public const int MinSavingPercentDivisor = 100;

    const uint DosDate = (0 << 9) | (1 << 5) | 1; // 1980-01-01
    const uint DosTime = 0;                       // 00:00:00
    const uint SigLocal = 0x04034b50, SigCentral = 0x02014b50, SigEnd = 0x06054b50;

    static readonly uint[] CrcTable = BuildCrcTable();

    static uint[] BuildCrcTable()
    {
        var table = new uint[256];
        for (uint n = 0; n < 256; n++)
        {
            uint c = n;
            for (int k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return table;
    }

    static uint CrcUpdate(uint crc, byte[] buffer, int offset, int count)
    {
        uint c = crc ^ 0xFFFFFFFFu;
        for (int i = offset; i < offset + count; i++) c = CrcTable[(c ^ buffer[i]) & 0xFF] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    public static string Hex(byte[] bytes)
    {
        var sb = new StringBuilder(bytes.Length * 2);
        foreach (byte b in bytes) sb.Append(b.ToString("x2", CultureInfo.InvariantCulture));
        return sb.ToString();
    }

    public static string Sha256Text(string text)
    {
        using (var sha = SHA256.Create()) return Hex(sha.ComputeHash(Encoding.UTF8.GetBytes(text)));
    }

    // One streaming pass: SHA-256 and CRC-32 of a file.
    public static void HashFile(string path, out string sha256, out uint crc, out long length)
    {
        var buffer = new byte[1 << 20];
        uint c = 0;
        long total = 0;
        using (var sha = SHA256.Create())
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
        {
            int read;
            while ((read = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                sha.TransformBlock(buffer, 0, read, null, 0);
                c = CrcUpdate(c, buffer, 0, read);
                total += read;
            }
            sha.TransformFinalBlock(buffer, 0, 0);
            sha256 = Hex(sha.Hash);
        }
        crc = c;
        length = total;
    }

    public static void HashBytes(byte[] bytes, out string sha256, out uint crc)
    {
        using (var sha = SHA256.Create()) sha256 = Hex(sha.ComputeHash(bytes));
        crc = CrcUpdate(0, bytes, 0, bytes.Length);
    }

    public static void ValidateName(string name)
    {
        if (string.IsNullOrEmpty(name) || name[0] == '/' || name.EndsWith("/", StringComparison.Ordinal))
            throw new ArgumentException("Invalid pack entry name: '" + name + "'");
        foreach (char ch in name)
        {
            if (ch < 0x20 || ch > 0x7E || ch == '\\' || ch == ':')
                throw new ArgumentException("Pack entry name is not portable ASCII: '" + name + "'");
        }
        foreach (string part in name.Split('/'))
        {
            if (part.Length == 0 || part == "." || part == "..")
                throw new ArgumentException("Pack entry name has an empty or relative segment: '" + name + "'");
        }
    }

    static bool Better(long size, long compressed)
    {
        return size - compressed >= Math.Max(MinSavingBytes, size / MinSavingPercentDivisor);
    }

    static byte[] Deflate(byte[] raw)
    {
        using (var ms = new MemoryStream())
        {
            using (var deflate = new DeflateStream(ms, CompressionLevel.Optimal, true)) deflate.Write(raw, 0, raw.Length);
            return ms.ToArray();
        }
    }

    static string CacheMetaPath(string cacheDir, string key) { return Path.Combine(cacheDir, key + ".meta"); }
    static string CachePayloadPath(string cacheDir, string key) { return Path.Combine(cacheDir, key + ".bin"); }

    static string CacheKey(string sha256, string platform, bool forceStored)
    {
        return Sha256Text(sha256 + "|" + platform + "|" + PolicyVersion + "|" + ToolVersion + (forceStored ? "|stored" : "|adaptive"));
    }

    static Dictionary<string, string> ReadMeta(string path)
    {
        var fields = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (string line in File.ReadAllLines(path, Encoding.ASCII))
        {
            int eq = line.IndexOf('=');
            if (eq <= 0) throw new InvalidDataException("Malformed cache metadata");
            fields[line.Substring(0, eq)] = line.Substring(eq + 1);
        }
        return fields;
    }

    static void WriteAtomic(string path, string text)
    {
        string tmp = path + ".tmp-" + Guid.NewGuid().ToString("N");
        File.WriteAllText(tmp, text, Encoding.ASCII);
        ReplaceFile(tmp, path);
    }

    static void ReplaceFile(string tmp, string path)
    {
        if (File.Exists(path)) File.Replace(tmp, path, null);
        else File.Move(tmp, path);
    }

    // Returns a prepared entry, using the cache when a verified entry exists.
    static NoxPackEntry Prepare(NoxPackInput input, string cacheDir, string platform)
    {
        ValidateName(input.Name);
        string sha; uint crc; long size;
        if (input.Bytes != null)
        {
            size = input.Bytes.Length;
            HashBytes(input.Bytes, out sha, out crc);
        }
        else
        {
            if (!File.Exists(input.Path)) throw new FileNotFoundException("Pack source missing", input.Path);
            HashFile(input.Path, out sha, out crc, out size);
        }

        string key = CacheKey(sha, platform, input.ForceStored);
        string metaPath = CacheMetaPath(cacheDir, key);
        string payloadPath = CachePayloadPath(cacheDir, key);
        var entry = new NoxPackEntry { Name = input.Name, Size = size, Crc = crc, Sha256 = sha };
        bool recovered = false;

        if (File.Exists(metaPath))
        {
            if (TryLoadCacheEntry(entry, metaPath, payloadPath, input)) { entry.CacheHit = true; return entry; }
            // Corrupt or stale entry: discard it and cook again.
            File.Delete(metaPath);
            if (File.Exists(payloadPath)) File.Delete(payloadPath);
            recovered = true;
        }

        byte[] raw = input.Bytes ?? File.ReadAllBytes(input.Path);
        if (raw.Length != size) throw new IOException("Pack source changed while cooking: " + input.Name);
        byte[] compressed = input.ForceStored ? null : Deflate(raw);
        if (compressed != null && Better(size, compressed.Length))
        {
            entry.Method = 8;
            entry.CompressedSize = compressed.Length;
            Directory.CreateDirectory(cacheDir);
            string tmp = payloadPath + ".tmp-" + Guid.NewGuid().ToString("N");
            File.WriteAllBytes(tmp, compressed);
            ReplaceFile(tmp, payloadPath);
            entry.DataPath = payloadPath;
            string payloadSha; uint ignored; long ignoredLen;
            HashFile(payloadPath, out payloadSha, out ignored, out ignoredLen);
            WriteAtomic(metaPath, FormatMeta(entry, payloadSha));
        }
        else
        {
            entry.Method = 0;
            entry.CompressedSize = size;
            if (input.Bytes != null) entry.DataBytes = input.Bytes;
            else entry.DataPath = input.Path;
            Directory.CreateDirectory(cacheDir);
            WriteAtomic(metaPath, FormatMeta(entry, ""));
        }
        entry.Recovered = recovered;
        return entry;
    }

    static string FormatMeta(NoxPackEntry e, string payloadSha)
    {
        var sb = new StringBuilder();
        sb.Append("format=1\n");
        sb.Append("method=" + e.Method.ToString(CultureInfo.InvariantCulture) + "\n");
        sb.Append("crc=" + e.Crc.ToString("x8", CultureInfo.InvariantCulture) + "\n");
        sb.Append("size=" + e.Size.ToString(CultureInfo.InvariantCulture) + "\n");
        sb.Append("csize=" + e.CompressedSize.ToString(CultureInfo.InvariantCulture) + "\n");
        sb.Append("sha256=" + e.Sha256 + "\n");
        sb.Append("payload_sha256=" + payloadSha + "\n");
        return sb.ToString();
    }

    static bool TryLoadCacheEntry(NoxPackEntry entry, string metaPath, string payloadPath, NoxPackInput input)
    {
        try
        {
            var m = ReadMeta(metaPath);
            if (m["format"] != "1" || m["sha256"] != entry.Sha256 || m["crc"] != entry.Crc.ToString("x8", CultureInfo.InvariantCulture) ||
                m["size"] != entry.Size.ToString(CultureInfo.InvariantCulture)) return false;
            int method = int.Parse(m["method"], CultureInfo.InvariantCulture);
            long csize = long.Parse(m["csize"], CultureInfo.InvariantCulture);
            if (method == 0)
            {
                if (csize != entry.Size) return false;
                entry.Method = 0;
                entry.CompressedSize = csize;
                if (input.Bytes != null) entry.DataBytes = input.Bytes;
                else entry.DataPath = input.Path;
                return true;
            }
            if (method != 8 || !File.Exists(payloadPath) || new FileInfo(payloadPath).Length != csize) return false;
            string payloadSha; uint crc; long len;
            HashFile(payloadPath, out payloadSha, out crc, out len);
            if (payloadSha != m["payload_sha256"]) return false;
            entry.Method = 8;
            entry.CompressedSize = csize;
            entry.DataPath = payloadPath;
            return true;
        }
        catch (Exception)
        {
            return false;
        }
    }

    // Plans, reuses or cooks every entry, then writes the pack only when its index changed.
    public static NoxPackResult Build(string packPath, string cacheDir, string platform, List<NoxPackInput> inputs)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var sorted = new List<NoxPackInput>(inputs);
        sorted.Sort(delegate (NoxPackInput a, NoxPackInput b) { return string.CompareOrdinal(a.Name, b.Name); });
        var result = new NoxPackResult { Path = packPath };
        foreach (NoxPackInput input in sorted)
        {
            ValidateName(input.Name);
            if (!seen.Add(input.Name))
                throw new ArgumentException("Duplicate or case-colliding pack entry: " + input.Name);
            result.Entries.Add(Prepare(input, cacheDir, platform));
        }
        foreach (NoxPackEntry e in result.Entries)
        {
            if (e.CacheHit) result.CacheHits++; else result.Cooked++;
            if (e.Recovered) result.Recovered++;
        }
        if (result.Entries.Count >= 65535) throw new InvalidOperationException("Too many entries for a non-ZIP64 pack.");

        // Planned offsets are a pure function of names and sizes.
        long offset = 0;
        foreach (NoxPackEntry e in result.Entries)
        {
            e.Offset = offset;
            offset += 30 + Encoding.ASCII.GetByteCount(e.Name) + e.CompressedSize;
        }
        long centralStart = offset;
        long centralSize = 0;
        foreach (NoxPackEntry e in result.Entries) centralSize += 46 + Encoding.ASCII.GetByteCount(e.Name);
        result.FileBytes = centralStart + centralSize + 22;
        result.IndexSha256 = IndexHash(result.Entries);

        if (File.Exists(packPath) && IndexMatches(packPath, result.Entries))
        {
            result.Rewritten = false;
            return result;
        }

        string dir = Path.GetDirectoryName(Path.GetFullPath(packPath));
        Directory.CreateDirectory(dir);
        string tmp = packPath + ".tmp-" + Guid.NewGuid().ToString("N");
        try
        {
            WriteArchive(tmp, result.Entries, centralStart);
            VerifyArchive(tmp, result.Entries, result.FileBytes);
            ReplaceFile(tmp, packPath);
        }
        catch
        {
            if (File.Exists(tmp)) File.Delete(tmp);
            throw;
        }
        result.Rewritten = true;
        result.FileBytes = new FileInfo(packPath).Length;
        return result;
    }

    public static string IndexHash(List<NoxPackEntry> entries)
    {
        var sb = new StringBuilder();
        foreach (NoxPackEntry e in entries)
        {
            sb.Append(e.Name).Append('|').Append(e.Method).Append('|').Append(e.Crc.ToString("x8", CultureInfo.InvariantCulture))
              .Append('|').Append(e.Size).Append('|').Append(e.CompressedSize).Append('|').Append(e.Offset)
              .Append('|').Append(e.Sha256).Append('\n');
        }
        return Sha256Text(sb.ToString());
    }

    static void WriteArchive(string path, List<NoxPackEntry> entries, long centralStart)
    {
        using (var fs = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 16))
        using (var w = new BinaryWriter(fs, Encoding.ASCII))
        {
            foreach (NoxPackEntry e in entries)
            {
                byte[] name = Encoding.ASCII.GetBytes(e.Name);
                w.Write(SigLocal);
                w.Write((ushort)20);            // version needed: 2.0
                w.Write((ushort)0);             // flags: no descriptor, no encryption
                w.Write((ushort)e.Method);
                w.Write((ushort)DosTime);
                w.Write((ushort)DosDate);
                w.Write(e.Crc);
                w.Write((uint)e.CompressedSize);
                w.Write((uint)e.Size);
                w.Write((ushort)name.Length);
                w.Write((ushort)0);             // no extra field
                w.Write(name);
                CopyPayload(e, fs);
            }
            if (fs.Position != centralStart) throw new IOException("Internal error: archive layout mismatch");
            foreach (NoxPackEntry e in entries)
            {
                byte[] name = Encoding.ASCII.GetBytes(e.Name);
                w.Write(SigCentral);
                w.Write((ushort)20);            // version made by (MS-DOS, 2.0)
                w.Write((ushort)20);            // version needed
                w.Write((ushort)0);
                w.Write((ushort)e.Method);
                w.Write((ushort)DosTime);
                w.Write((ushort)DosDate);
                w.Write(e.Crc);
                w.Write((uint)e.CompressedSize);
                w.Write((uint)e.Size);
                w.Write((ushort)name.Length);
                w.Write((ushort)0);             // extra
                w.Write((ushort)0);             // comment
                w.Write((ushort)0);             // disk
                w.Write((ushort)0);             // internal attributes
                w.Write((uint)0);               // external attributes
                w.Write((uint)e.Offset);
                w.Write(name);
            }
            long centralSize = fs.Position - centralStart;
            w.Write(SigEnd);
            w.Write((ushort)0);
            w.Write((ushort)0);
            w.Write((ushort)entries.Count);
            w.Write((ushort)entries.Count);
            w.Write((uint)centralSize);
            w.Write((uint)centralStart);
            w.Write((ushort)0);
            w.Flush();
        }
    }

    static void CopyPayload(NoxPackEntry e, Stream output)
    {
        if (e.DataBytes != null)
        {
            if (e.DataBytes.LongLength != e.CompressedSize) throw new IOException("Payload size mismatch: " + e.Name);
            output.Write(e.DataBytes, 0, e.DataBytes.Length);
            return;
        }
        var buffer = new byte[1 << 20];
        long copied = 0;
        using (var input = new FileStream(e.DataPath, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
        {
            if (input.Length != e.CompressedSize) throw new IOException("Source changed while packing: " + e.Name);
            int read;
            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
            {
                output.Write(buffer, 0, read);
                copied += read;
            }
        }
        if (copied != e.CompressedSize) throw new IOException("Short read while packing: " + e.Name);
    }

    sealed class Central
    {
        public string Name;
        public int Method;
        public uint Crc;
        public long CompressedSize;
        public long Size;
        public long Offset;
    }

    static List<Central> ReadCentral(BinaryReader r, long fileLength)
    {
        if (fileLength < 22) throw new InvalidDataException("Archive too small");
        r.BaseStream.Seek(fileLength - 22, SeekOrigin.Begin);
        if (r.ReadUInt32() != SigEnd) throw new InvalidDataException("Missing end-of-central-directory (or archive comment present)");
        r.ReadUInt16(); r.ReadUInt16();
        int count = r.ReadUInt16();
        int countTotal = r.ReadUInt16();
        long cdSize = r.ReadUInt32();
        long cdOffset = r.ReadUInt32();
        if (r.ReadUInt16() != 0 || count != countTotal || cdOffset + cdSize + 22 != fileLength)
            throw new InvalidDataException("Inconsistent end-of-central-directory");
        r.BaseStream.Seek(cdOffset, SeekOrigin.Begin);
        var list = new List<Central>(count);
        for (int i = 0; i < count; i++)
        {
            if (r.ReadUInt32() != SigCentral) throw new InvalidDataException("Bad central directory header");
            r.ReadUInt16(); r.ReadUInt16(); r.ReadUInt16();
            var c = new Central();
            c.Method = r.ReadUInt16();
            r.ReadUInt16(); r.ReadUInt16();
            c.Crc = r.ReadUInt32();
            c.CompressedSize = r.ReadUInt32();
            c.Size = r.ReadUInt32();
            int nameLength = r.ReadUInt16();
            int extra = r.ReadUInt16();
            int comment = r.ReadUInt16();
            r.ReadUInt16(); r.ReadUInt16(); r.ReadUInt32();
            c.Offset = r.ReadUInt32();
            if (extra != 0 || comment != 0) throw new InvalidDataException("Unexpected extra field or comment");
            c.Name = Encoding.ASCII.GetString(r.ReadBytes(nameLength));
            list.Add(c);
        }
        return list;
    }

    // True when the archive's central directory equals the planned index exactly.
    static bool IndexMatches(string path, List<NoxPackEntry> entries)
    {
        try
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
            using (var r = new BinaryReader(fs, Encoding.ASCII))
            {
                List<Central> central = ReadCentral(r, fs.Length);
                if (central.Count != entries.Count) return false;
                for (int i = 0; i < entries.Count; i++)
                {
                    NoxPackEntry e = entries[i];
                    Central c = central[i];
                    if (c.Name != e.Name || c.Method != e.Method || c.Crc != e.Crc || c.Size != e.Size ||
                        c.CompressedSize != e.CompressedSize || c.Offset != e.Offset) return false;
                }
                return true;
            }
        }
        catch (Exception)
        {
            return false;
        }
    }

    // Re-reads the written temp archive: central directory, local headers, payload hashes.
    static void VerifyArchive(string path, List<NoxPackEntry> entries, long expectedLength)
    {
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var r = new BinaryReader(fs, Encoding.ASCII))
        {
            if (fs.Length != expectedLength) throw new InvalidDataException("Archive length differs from plan");
            List<Central> central = ReadCentral(r, fs.Length);
            if (central.Count != entries.Count) throw new InvalidDataException("Entry count differs from plan");
            for (int i = 0; i < entries.Count; i++)
            {
                NoxPackEntry e = entries[i];
                Central c = central[i];
                if (c.Name != e.Name || c.Method != e.Method || c.Crc != e.Crc || c.Size != e.Size ||
                    c.CompressedSize != e.CompressedSize || c.Offset != e.Offset)
                    throw new InvalidDataException("Central directory differs from plan: " + e.Name);
                fs.Seek(c.Offset, SeekOrigin.Begin);
                if (r.ReadUInt32() != SigLocal) throw new InvalidDataException("Bad local header: " + e.Name);
                r.ReadUInt16();
                if (r.ReadUInt16() != 0 || r.ReadUInt16() != e.Method) throw new InvalidDataException("Local header flags/method differ: " + e.Name);
                r.ReadUInt16(); r.ReadUInt16();
                if (r.ReadUInt32() != e.Crc || r.ReadUInt32() != e.CompressedSize || r.ReadUInt32() != e.Size)
                    throw new InvalidDataException("Local header sizes differ: " + e.Name);
                if (r.ReadUInt16() != Encoding.ASCII.GetByteCount(e.Name) || r.ReadUInt16() != 0)
                    throw new InvalidDataException("Local header name/extra differ: " + e.Name);
                if (Encoding.ASCII.GetString(r.ReadBytes(Encoding.ASCII.GetByteCount(e.Name))) != e.Name)
                    throw new InvalidDataException("Local header name differs: " + e.Name);
                byte[] payload = r.ReadBytes((int)e.CompressedSize);
                if (payload.Length != e.CompressedSize) throw new InvalidDataException("Truncated payload: " + e.Name);
                byte[] raw;
                if (e.Method == 0) raw = payload;
                else if (e.Method == 8)
                {
                    using (var input = new MemoryStream(payload))
                    using (var inflate = new DeflateStream(input, CompressionMode.Decompress))
                    using (var output = new MemoryStream())
                    {
                        inflate.CopyTo(output);
                        raw = output.ToArray();
                    }
                }
                else throw new InvalidDataException("Unsupported method " + e.Method + ": " + e.Name);
                if (raw.LongLength != e.Size) throw new InvalidDataException("Inflated size differs: " + e.Name);
                uint crc = CrcUpdate(0, raw, 0, raw.Length);
                string sha; uint ignored;
                HashBytes(raw, out sha, out ignored);
                if (crc != e.Crc || sha != e.Sha256) throw new InvalidDataException("Payload hash differs: " + e.Name);
            }
        }
    }

    // Reads the central directory of an existing archive (names, methods, sizes, offsets).
    public static List<string> ReadIndexLines(string path)
    {
        var lines = new List<string>();
        using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read))
        using (var r = new BinaryReader(fs, Encoding.ASCII))
        {
            foreach (Central c in ReadCentral(r, fs.Length))
                lines.Add(c.Name + "|" + c.Method + "|" + c.Crc.ToString("x8", CultureInfo.InvariantCulture) + "|" + c.Size + "|" + c.CompressedSize + "|" + c.Offset);
        }
        return lines;
    }
}
