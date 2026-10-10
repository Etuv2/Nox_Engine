# Shuto Expressway route manifest (reference date 2002-12-31)

Companion to `route_manifest.json` in this folder. The JSON holds the full route, segment, junction, interchange, parking, boundary and landmark data. This file lists sources and summarizes status.

## Sources (accessed 2026-10-10)

| id | Title | URL | Status |
|---|---|---|---|
| S1 | Shuto Expressway (English Wikipedia) | https://en.wikipedia.org/wiki/Shuto_Expressway | Fetched; route list and 1962 first section only |
| S2 | Japanese Wikipedia, route overview and history (首都高速道路) | https://ja.wikipedia.org/wiki/首都高速道路 | Fetched; dated section list for all routes |
| S3 | Japanese Wikipedia, Inner Circular Route (C1) | https://ja.wikipedia.org/wiki/首都高速都心環状線 | Fetched |
| S4 | Japanese Wikipedia, Central Circular Route (C2) | https://ja.wikipedia.org/wiki/首都高速中央環状線 | Fetched |
| S5 | Japanese Wikipedia, 1 Haneda Route | https://ja.wikipedia.org/wiki/首都高速1号羽田線 | Fetched |
| S6 | Japanese Wikipedia, 1 Ueno Route | https://ja.wikipedia.org/wiki/首都高速1号上野線 | Fetched |
| S7 | Japanese Wikipedia, 2 Meguro Route | https://ja.wikipedia.org/wiki/首都高速2号目黒線 | Fetched |
| S8 | Japanese Wikipedia, 3 Shibuya Route | https://ja.wikipedia.org/wiki/首都高速3号渋谷線 | Fetched |
| S9 | Japanese Wikipedia, 4 Shinjuku Route | https://ja.wikipedia.org/wiki/首都高速4号新宿線 | Fetched |
| S10 | Japanese Wikipedia, 5 Ikebukuro Route | https://ja.wikipedia.org/wiki/首都高速5号池袋線 | Fetched |
| S11 | Japanese Wikipedia, 6 Mukojima Route | https://ja.wikipedia.org/wiki/首都高速6号向島線 | Fetched |
| S12 | Japanese Wikipedia, 6 Misato Route | https://ja.wikipedia.org/wiki/首都高速6号三郷線 | Fetched |
| S13 | Japanese Wikipedia, 7 Komatsugawa Route | https://ja.wikipedia.org/wiki/首都高速7号小松川線 | Fetched |
| S14 | Japanese Wikipedia, 9 Fukagawa Route | https://ja.wikipedia.org/wiki/首都高速9号深川線 | Fetched |
| S15 | Japanese Wikipedia, 11 Daiba Route | https://ja.wikipedia.org/wiki/首都高速11号台場線 | Fetched |
| S16 | Japanese Wikipedia, Wangan Route (B) | https://ja.wikipedia.org/wiki/首都高速湾岸線 | Fetched |
| S17 | Japanese Wikipedia, Kanagawa 1 Yokohane Route | https://ja.wikipedia.org/wiki/首都高速神奈川1号横羽線 | Fetched |
| S18 | Japanese Wikipedia, Kanagawa 5 Daikoku Route | https://ja.wikipedia.org/wiki/首都高速神奈川5号大黒線 | Fetched |
| S19 | Japanese Wikipedia, S1 Kawaguchi Route | https://ja.wikipedia.org/wiki/首都高速川口線 | Fetched |
| S20 | Japanese Wikipedia, S5 Omiya Route | https://ja.wikipedia.org/wiki/首都高速大宮線 | HTTP 404; not obtained |
| S21 | Japanese Wikipedia, Kawasaki Route | https://ja.wikipedia.org/wiki/首都高速川崎線 | HTTP 404; not obtained |
| S22 | Metropolitan Expressway, route page | https://www.shutoko.jp/driving/route/ | HTTP 429 twice; not used |
| S23 | Metropolitan Expressway, navi-map | https://www.shutoko.jp/driving/navi-map/ | Not fetched |

## Status summary

- **In era (open at 2002-12-31):** C1 (complete 1967), C2 partial (1982-2002, Itabashi-Kohoku 2002-12-25), B Wangan (complete 2001-10-22), 1 Haneda (1963-1966), 1 Ueno (1969), 2 Meguro (1967), 3 Shibuya (1971), 4 Shinjuku (1976), 5 Ikebukuro (1967-1993, Bijogi JCT), 6 Mukojima (1971-1982), 6 Misato (1985), 7 Komatsugawa (1971), 9 Fukagawa (1980), 11 Daiba (1993), Y Yaesu (1964/1973), K1 Yokohane (1968-1984), K3 Kariba (1984/1990), K5 Daikoku (1989), K6 Kawasaki (partial, 2002-04-30), S1 Kawaguchi (1987), S5 Omiya (1998, from overview only).
- **Excluded by date (opened after 2002-12-31):** C2 west and Yamate Tunnel (Nishi-Shinjuku 2007, Ohashi 2010, Oi-Ohashi 2015); B Honmoku JCT (2004), Shinonome JCT (2009), Minami-Honmoku IC (2017), Takaya-Outer Ring link (2018); 10 Harumi (2009/2018); K7 Yokohama North (2017/2020); K6 Daishi-Tonomachi (2010); K1 Daishi entries (2009/2010); S2 Shintoshin main sections (2004/2006); Komatsugawa JCT on C2 and 7 (2019).
- **Open at reference, closed later:** Y Yaesu Route (closed 2025-04-05, long-term underground works); C1 Tokyo Expressway link (2025-04-05); C1 Gofukubashi and Edobashi exits (2021-05-10).

## Key uncertainties

1. **Official data not verified.** shutoko.jp returned HTTP 429 and the navi-map was not fetched. All dates come from Wikipedia-derived pages.
2. **Fetched text was summarized by a small model.** It contains internal inconsistencies. Conflicts are logged in the JSON `verification_notes`. Examples: 5-Ikebukuro 1977 and 1990 section lengths, 9-Fukagawa total length, 1-Haneda section lengths, 6-Mukojima 1971 length, S1 start point, and the K1/K2 assignment of the 1978 section.
3. **K2 Mitsuzawa:** no verified section data.
4. **S5 Omiya and K6 Kawasaki:** per-route pages returned 404. Data comes only from the overview page, so treat as unverified.
5. **English names** for junctions and interchanges are transliterations, not official names.
6. **Lengths** are null where no source states them. Some were derived from km markers and are approximate.
7. **Operator names** for external connections (Tomei, Chuo, Higashi-Kanto, Joban, Tokyo Gaikan, Keiyo, Yokohama-Yokosuka) are from general knowledge, not from the fetched sources.
