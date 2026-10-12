#include "RacingHUD.h"
#include "RoadNetwork.h"
#include "UiIconAtlas.h"
#include "assets/AssetIO.h"
#include <cstring>
#include <optional>
#include "ui/Pointer.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr float kRefW = 640.0f;
constexpr float kRefH = 360.0f;
// Tachometer / gauge sweep: from the lower-left (150 deg) clockwise over the top to the lower-right.
constexpr float kSweepStart = 150.0f * kPi / 180.0f;
constexpr float kSweep = 240.0f * kPi / 180.0f;

float Clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
float ClampF(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float Damp(float cur, float target, float rate, float dt) { return cur + (target - cur) * (1.0f - std::exp(-rate * dt)); }
float WrapAngle(float a) {
	while (a > kPi) a -= 2.0f * kPi;
	while (a < -kPi) a += 2.0f * kPi;
	return a;
}
ImVec2 Dir(float a) { return ImVec2(std::cos(a), std::sin(a)); }
ImVec2 Add(ImVec2 a, ImVec2 b) { return ImVec2(a.x + b.x, a.y + b.y); }
ImVec2 Mul(ImVec2 a, float s) { return ImVec2(a.x * s, a.y * s); }
float Dot(ImVec2 a, ImVec2 b) { return a.x * b.x + a.y * b.y; }

ImU32 Col(const glm::vec4& c, float alpha = 1.0f) {
	const float a = Clamp01(c.a * alpha);
	return IM_COL32((int)std::lround(Clamp01(c.r) * 255.0f), (int)std::lround(Clamp01(c.g) * 255.0f),
		(int)std::lround(Clamp01(c.b) * 255.0f), (int)std::lround(a * 255.0f));
}
ImU32 ColA(float r, float g, float b, float a) {
	return IM_COL32((int)std::lround(Clamp01(r) * 255.0f), (int)std::lround(Clamp01(g) * 255.0f),
		(int)std::lround(Clamp01(b) * 255.0f), (int)std::lround(Clamp01(a) * 255.0f));
}

// Maps reference-canvas coordinates (640x360) to screen pixels for one element.
struct Xf {
	ImVec2 base{ 0, 0 };
	ImVec2 refAnchor{ 0, 0 };
	ImVec2 off{ 0, 0 };
	float s = 1.0f;
	ImVec2 P(float x, float y) const {
		return ImVec2(base.x + (x - refAnchor.x + off.x) * s, base.y + (y - refAnchor.y + off.y) * s);
	}
	float L(float v) const { return v * s; }
};

void ParseAnchor(const std::string& a, float& fx, float& fy) {
	fx = 0.0f;
	fy = 0.0f;
	if (a.size() < 2) return;
	const char v = (char)std::toupper((unsigned char)a[0]);
	const char h = (char)std::toupper((unsigned char)a[1]);
	if (v == 'M') fy = 0.5f; else if (v == 'B') fy = 1.0f;
	if (h == 'C') fx = 0.5f; else if (h == 'R') fx = 1.0f;
}

Xf MakeXf(const ImVec2& origin, const ImVec2& size, const HudPlacement& p, float H) {
	float fx, fy;
	ParseAnchor(p.anchor, fx, fy);
	Xf x;
	x.refAnchor = ImVec2(fx * kRefW, fy * kRefH);
	x.base = ImVec2(origin.x + fx * size.x, origin.y + fy * size.y);
	x.off = ImVec2(p.offset.x, p.offset.y);
	x.s = H / kRefH * p.scale;
	return x;
}

// Glow stroke: wide low-alpha layers under a sharp core.
void Stroke(ImDrawList* dl, const std::vector<ImVec2>& pts, const glm::vec4& c, float thick, float glow, float alpha = 1.0f) {
	if (pts.size() < 2 || thick <= 0.0f) return;
	const int n = (int)pts.size();
	if (glow > 0.0f) {
		dl->AddPolyline(pts.data(), n, Col(c, 0.08f * glow * alpha), ImDrawFlags_None, thick * 4.5f);
		dl->AddPolyline(pts.data(), n, Col(c, 0.20f * glow * alpha), ImDrawFlags_None, thick * 2.2f);
	}
	dl->AddPolyline(pts.data(), n, Col(c, alpha), ImDrawFlags_None, thick);
}
void Seg(ImDrawList* dl, ImVec2 a, ImVec2 b, const glm::vec4& c, float thick, float glow, float alpha = 1.0f) {
	const std::vector<ImVec2> p{ a, b };
	Stroke(dl, p, c, thick, glow, alpha);
}
std::vector<ImVec2> ArcPts(ImVec2 c, float r, float a0, float a1, int n) {
	std::vector<ImVec2> out;
	n = std::max(2, n);
	out.reserve((size_t)n + 1);
	for (int i = 0; i <= n; ++i) {
		const float a = a0 + (a1 - a0) * (float)i / (float)n;
		out.push_back(Add(c, Mul(Dir(a), r)));
	}
	return out;
}

// Clips segment a..b against a circle. On success t0 < t1 give the visible parameter range.
bool ClipSegCircle(ImVec2 a, ImVec2 b, ImVec2 c, float r, float& t0, float& t1) {
	const ImVec2 d(b.x - a.x, b.y - a.y);
	const ImVec2 f(a.x - c.x, a.y - c.y);
	const float A = Dot(d, d);
	t0 = 0.0f;
	t1 = 1.0f;
	if (A < 1e-9f) return Dot(f, f) <= r * r;
	const float B = 2.0f * Dot(d, f);
	const float C = Dot(f, f) - r * r;
	const float disc = B * B - 4.0f * A * C;
	if (disc < 0.0f) return false;
	const float sq = std::sqrt(disc);
	const float ta = (-B - sq) / (2.0f * A);
	const float tb = (-B + sq) / (2.0f * A);
	t0 = std::max(0.0f, ta);
	t1 = std::min(1.0f, tb);
	return t0 < t1;
}

// Text helper. align: 0 left, 1 centre, 2 right. vcentre centres the line box on p.y.
void Txt(ImDrawList* dl, ImFont* f, float px, ImVec2 p, ImU32 col, const char* s, int align = 0, bool vcentre = true) {
	if (!dl || !f || !s || !*s || px <= 0.5f) return;
	const ImVec2 sz = f->CalcTextSizeA(px, FLT_MAX, 0.0f, s);
	float x = p.x;
	if (align == 1) x -= sz.x * 0.5f;
	else if (align == 2) x -= sz.x;
	const float y = vcentre ? p.y - sz.y * 0.5f : p.y;
	dl->AddText(f, px, ImVec2(x, y), col, s);
}
void TxtGlow(ImDrawList* dl, ImFont* f, float px, ImVec2 p, const glm::vec4& c, const char* s, int align, float glow, float alpha = 1.0f) {
	if (glow > 0.0f && f && s && *s) {
		const float d = std::max(0.6f, px * 0.05f);
		const ImU32 g = Col(c, 0.14f * glow * alpha);
		const float offs[8][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }, { 0.7f, 0.7f }, { -0.7f, 0.7f }, { 0.7f, -0.7f }, { -0.7f, -0.7f } };
		for (const auto& o : offs) Txt(dl, f, px, ImVec2(p.x + o[0] * d, p.y + o[1] * d), g, s, align);
	}
	Txt(dl, f, px, p, Col(c, alpha), s, align);
}

ImFont* PickFont(const std::vector<ImFont*>& fonts, const std::vector<float>& sizes, float px) {
	if (fonts.empty()) return nullptr;
	size_t best = 0;
	float bestD = FLT_MAX;
	for (size_t i = 0; i < fonts.size() && i < sizes.size(); ++i) {
		const float d = std::fabs(sizes[i] - px);
		if (d < bestD) { bestD = d; best = i; }
	}
	return fonts[best];
}

std::string FormatTime(float s) {
	if (s < 0.0f) return "--:--.--";
	const int cs = (int)std::lround(s * 100.0f);
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%02d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
	return buf;
}

// Tolerant JSON readers: a missing or mistyped key keeps the current (default) value.
template <class T>
void Read(const nlohmann::json& j, const char* sec, const char* key, T& out) {
	try {
		const nlohmann::json* p = &j;
		if (sec) {
			if (!j.is_object() || !j.contains(sec)) return;
			p = &j[sec];
		}
		if (!p->is_object() || !p->contains(key)) return;
		out = (*p)[key].template get<T>();
	} catch (...) {
	}
}
void ReadVec(const nlohmann::json& j, const char* sec, const char* key, glm::vec4& out) {
	try {
		if (!j.is_object() || !j.contains(sec) || !j[sec].is_object() || !j[sec].contains(key)) return;
		const auto& a = j[sec][key];
		if (!a.is_array()) return;
		float v[4] = { out.r, out.g, out.b, out.a };
		for (size_t i = 0; i < 4 && i < a.size(); ++i) v[i] = a[i].get<float>();
		out = glm::vec4(v[0], v[1], v[2], v[3]);
	} catch (...) {
	}
}
void ReadPlacement(const nlohmann::json& j, const char* name, HudPlacement& p) {
	try {
		if (!j.is_object() || !j.contains("placement") || !j["placement"].is_object() || !j["placement"].contains(name)) return;
		const auto& o = j["placement"][name];
		if (o.contains("anchor")) p.anchor = o["anchor"].get<std::string>();
		if (o.contains("offset") && o["offset"].is_array() && o["offset"].size() >= 2) {
			p.offset = glm::vec2(o["offset"][0].get<float>(), o["offset"][1].get<float>());
		}
		if (o.contains("scale")) p.scale = o["scale"].get<float>();
	} catch (...) {
	}
}

nlohmann::json Vec4Json(const glm::vec4& v) { return nlohmann::json::array({ v.r, v.g, v.b, v.a }); }
nlohmann::json Vec2Json(const glm::vec2& v) { return nlohmann::json::array({ v.x, v.y }); }

void InputStr(const char* label, std::string& s) {
	char buf[256] = {};
	std::snprintf(buf, sizeof(buf), "%s", s.c_str());
	if (ImGui::InputText(label, buf, sizeof(buf))) s = buf;
}
void EditPlacement(const char* name, HudPlacement& p) {
	ImGui::PushID(name);
	if (ImGui::TreeNode(name)) {
		char buf[8] = {};
		std::snprintf(buf, sizeof(buf), "%s", p.anchor.c_str());
		if (ImGui::InputText("anchor (TL TC TR ML MC MR BL BC BR)", buf, sizeof(buf))) p.anchor = buf;
		ImGui::DragFloat2("offset (ref px)", &p.offset.x, 0.5f, -400.0f, 400.0f);
		ImGui::SliderFloat("scale", &p.scale, 0.2f, 3.0f);
		ImGui::TreePop();
	}
	ImGui::PopID();
}

} // namespace

struct RacingHUD::Impl {
	HudConfig cfg;
	nlohmann::json raw = nlohmann::json::object();
	bool cfgLoaded = false;
	int saveStatus = 0; // 0 none, 1 saved, -1 failed

	std::vector<ImFont*> labelFonts, digitFonts;
	std::vector<float> labelSizes, digitSizes;

	// Animation state (damped toward the HudState values).
	bool animInit = false;
	float time = 0.0f;
	float needleRpm = 900.0f;
	float needleBoost = 0.0f;
	float needleN2o = 1.0f;
	float speedShown = 0.0f;
	float mapHeading = 0.0f;
	float mapRadius = 60.0f;

	// Per-frame context (valid inside Draw).
	ImDrawList* dl = nullptr;
	ImVec2 origin{ 0, 0 };
	ImVec2 size{ 0, 0 };
	float H = 1.0f;
	float dt = 0.0f;
	const HudState* st = nullptr;

	ImFont* LabelFont(float px) const { return PickFont(labelFonts, labelSizes, px); }
	ImFont* DigitFont(float px) const { return PickFont(digitFonts, digitSizes, px); }

	void Update() {
		const HudState& s = *st;
		const float boostT = Clamp01(s.load / std::max(0.01f, cfg.boostMax));
		const float n2oT = Clamp01(s.nitro);
		const float maxRpm = std::max(1000.0f, cfg.rpmMax);
		if (!animInit) {
			animInit = true;
			needleRpm = s.rpm;
			needleBoost = boostT;
			needleN2o = n2oT;
			speedShown = s.speedKmh;
			mapHeading = s.carHeading;
			mapRadius = cfg.mapRadiusM;
		}
		needleRpm = Damp(needleRpm, ClampF(s.rpm, 0.0f, maxRpm * 1.02f), 14.0f, dt);
		needleBoost = Damp(needleBoost, boostT, 9.0f, dt);
		needleN2o = Damp(needleN2o, n2oT, 6.0f, dt);
		const float kmh = (cfg.speedUnits == "mph") ? s.speedKmh * 0.621371f : s.speedKmh;
		speedShown = Damp(speedShown, kmh, 8.0f, dt);
		mapHeading += WrapAngle(s.carHeading - mapHeading) * (1.0f - std::exp(-8.0f * dt));
		const float targetRadius = cfg.mapRadiusM + std::max(0.0f, s.speedKmh) * cfg.mapZoomWithSpeed;
		mapRadius = Damp(mapRadius, targetRadius, 2.5f, dt);
	}

	// ---- Icon helpers (UiIconAtlas; every call falls back to the vector/text drawing) -----
	std::string Role(const std::string& key) const {
		auto it = cfg.iconRoles.find(key);
		return it == cfg.iconRoles.end() ? std::string() : it->second;
	}
	// Warning icon for a centre message whose text starts with a configured "warning/<text>" key.
	std::string MessageRole(const std::string& msg) const {
		for (const auto& kv : cfg.iconRoles) {
			if (kv.first.rfind("warning/", 0) != 0) continue;
			const std::string key = kv.first.substr(8);
			if (!key.empty() && msg.rfind(key, 0) == 0) return kv.second;
		}
		return std::string();
	}
	bool Icon(const std::string& id, ImVec2 c, float size, const glm::vec4& col, float alpha) {
		return UiIconAtlas::Get().DrawCentered(dl, id, c, size, Col(col, alpha));
	}
	// Icon whose right edge sits a small gap left of leftX (the text itself is not moved).
	bool IconLeftOf(const std::string& id, float leftX, float y, float size, const glm::vec4& col, float alpha) {
		const float gap = size * 0.3f;
		return Icon(id, ImVec2(leftX - gap - size * 0.5f, y), size, col, alpha);
	}
	static float TextW(ImFont* f, float px, const char* s) {
		return (f && s && *s) ? f->CalcTextSizeA(px, FLT_MAX, 0.0f, s).x : 0.0f;
	}

	// ---- Companion gauge (boost / N2O): arc, value arc and needle --------------------------
	void DrawCompanionGauge(const Xf& x, ImVec2 c, float R, float value, const glm::vec4& fillCol, float needleValue) {
		const float r = x.L(R);
		Stroke(dl, ArcPts(c, r * 0.97f, kSweepStart, kSweepStart + kSweep, 64), cfg.accent, x.L(0.5f), cfg.glow * 0.5f, 0.6f);
		for (int i = 0; i <= 10; ++i) {
			const float a = kSweepStart + kSweep * (float)i / 10.0f;
			Seg(dl, Add(c, Mul(Dir(a), r * 0.82f)), Add(c, Mul(Dir(a), r * 0.95f)), cfg.text, x.L(0.6f), 0.0f, 0.55f);
		}
		const float v = Clamp01(value);
		if (v > 0.001f) Stroke(dl, ArcPts(c, r * 0.97f, kSweepStart, kSweepStart + kSweep * v, 64), fillCol, x.L(1.1f), cfg.glow);
		const float na = kSweepStart + kSweep * Clamp01(needleValue);
		Seg(dl, Add(c, Mul(Dir(na), -r * 0.2f)), Add(c, Mul(Dir(na), r * 0.88f)), cfg.needle, x.L(1.1f), cfg.glow);
		dl->AddCircleFilled(c, x.L(2.2f), Col(cfg.needle));
	}

	// ---- Instrument cluster -----------------------------------------------------------------
	void DrawCluster() {
		const Xf x = MakeXf(origin, size, cfg.cluster, H);
		const float glow = cfg.glow;
		const float rpmMax = std::max(1000.0f, cfg.rpmMax);
		const float redFrac = Clamp01(cfg.redlineRpm / rpmMax);
		const bool redPulse = st->redline || st->limiter;
		const float pulse = redPulse ? (0.55f + 0.45f * std::sin(time * 18.0f)) : 0.0f;

		// Translucent dark backing.
		dl->AddRectFilled(x.P(460, 236), x.P(614, 356), ColA(0, 0, 0, cfg.backingAlpha), x.L(8.0f), ImDrawFlags_RoundCornersAll);

		// --- Tachometer ---
		const ImVec2 T = x.P(557, 291);
		const float R = x.L(45.0f);
		dl->AddCircleFilled(T, R * 1.02f, ColA(0, 0, 0, cfg.backingAlpha * 0.5f), 72);
		Stroke(dl, ArcPts(T, R * 0.97f, kSweepStart, kSweepStart + kSweep, 120), cfg.accent, x.L(0.6f), glow * 0.7f);
		Stroke(dl, ArcPts(T, R * 0.97f, kSweepStart + kSweep * redFrac, kSweepStart + kSweep, 80), cfg.redline, x.L(1.6f), glow, 0.75f + 0.25f * pulse);

		// Ticks: minor every 200 rpm, major every 1000 rpm.
		const int steps = std::max(1, (int)std::lround(rpmMax / 200.0f));
		for (int i = 0; i <= steps; ++i) {
			const float f = (float)i / (float)steps;
			const float a = kSweepStart + kSweep * f;
			const bool major = (i % 5) == 0;
			const bool hi = (float)i * 200.0f >= cfg.redlineRpm;
			const glm::vec4& tc = hi ? cfg.redline : cfg.text;
			const float r0 = R * (major ? 0.80f : 0.90f);
			const float r1 = R * (major ? 0.97f : 0.94f);
			Seg(dl, Add(T, Mul(Dir(a), r0)), Add(T, Mul(Dir(a), r1)), tc, x.L(major ? 1.3f : 0.6f), glow * 0.5f, hi ? (0.8f + 0.2f * pulse) : 0.85f);
		}
		// Numbered labels 0..N (x1000 rpm).
		const int labels = (int)std::floor(rpmMax / 1000.0f + 0.001f);
		for (int k = 0; k <= labels; ++k) {
			const float f = (float)k * 1000.0f / rpmMax;
			const ImVec2 p = Add(T, Mul(Dir(kSweepStart + kSweep * f), R * 0.64f));
			const bool hi = (float)k * 1000.0f >= cfg.redlineRpm;
			char buf[8];
			std::snprintf(buf, sizeof(buf), "%d", k);
			const float px = x.L(11.0f);
			TxtGlow(dl, LabelFont(px), px, p, hi ? cfg.redline : cfg.accent, buf, 1, glow * 0.6f, hi ? (0.85f + 0.15f * pulse) : 0.95f);
		}
		if (redPulse) dl->AddCircle(T, R * 1.04f, Col(cfg.redline, 0.35f * pulse), 72, x.L(1.5f));

		// Needle (damped) and hub.
		{
			const float a = kSweepStart + kSweep * Clamp01(needleRpm / rpmMax);
			Seg(dl, Add(T, Mul(Dir(a), -R * 0.22f)), Add(T, Mul(Dir(a), R * 0.92f)), cfg.needle, x.L(1.6f), glow * 1.2f);
			dl->AddCircleFilled(T, x.L(3.2f), ColA(0.08f, 0.02f, 0.02f, 1.0f));
			dl->AddCircle(T, x.L(3.2f), Col(cfg.needle), 16, x.L(0.9f));
		}

		// Digital speed box (blue) with unlit ghost segments.
		const ImVec2 b0 = x.P(537, 318), b1 = x.P(578, 334);
		dl->AddRectFilledMultiColor(b0, b1, Col(cfg.speed, 0.95f), Col(cfg.speed, 0.95f), Col(cfg.speed, 0.55f), Col(cfg.speed, 0.55f));
		dl->AddRect(b0, b1, Col(cfg.speed, 0.6f), x.L(1.5f), ImDrawFlags_RoundCornersAll, x.L(0.6f));
		const ImVec2 digitRight = x.P(575.5f, 326.5f);
		const float dpx = x.L(14.0f);
		Txt(dl, DigitFont(dpx), dpx, digitRight, ColA(0.04f, 0.10f, 0.22f, 0.12f), "888", 2);
		char spd[16];
		std::snprintf(spd, sizeof(spd), "%d", (int)std::lround(std::max(0.0f, speedShown)));
		Txt(dl, DigitFont(dpx), dpx, digitRight, ColA(0.03f, 0.08f, 0.20f, 1.0f), spd, 2);
		const float upx = x.L(7.0f);
		Txt(dl, LabelFont(upx), upx, x.P(557.5f, 340.5f), Col(cfg.speed, 0.9f), cfg.speedUnits.c_str(), 1);

		// Gear box.
		const ImVec2 g0 = x.P(516, 322), g1 = x.P(529, 339);
		dl->AddRectFilled(g0, g1, ColA(0, 0, 0, 0.55f), x.L(2.0f), ImDrawFlags_RoundCornersAll);
		if (redPulse) dl->AddRectFilled(g0, g1, Col(cfg.redline, 0.35f * pulse), x.L(2.0f), ImDrawFlags_RoundCornersAll);
		dl->AddRect(g0, g1, Col(redPulse ? cfg.redline : cfg.accent, 0.7f), x.L(2.0f), ImDrawFlags_RoundCornersAll, x.L(0.7f));
		char gear[4];
		if (st->gear < 0) std::snprintf(gear, sizeof(gear), "R");
		else if (st->gear == 0) std::snprintf(gear, sizeof(gear), "N");
		else std::snprintf(gear, sizeof(gear), "%d", st->gear);
		const float gpx = x.L(14.0f);
		TxtGlow(dl, LabelFont(gpx), gpx, x.P(522.5f, 330.5f), redPulse ? cfg.redline : cfg.text, gear, 1, glow * 0.5f);

		// --- Companion gauges: BOOST above, N2O below ---
		DrawCompanionGauge(x, x.P(481, 277), 21.0f, needleBoost, cfg.accent, needleBoost);
		const float blpx = x.L(6.5f);
		TxtGlow(dl, LabelFont(blpx), blpx, x.P(481, 288), cfg.accent, cfg.boostLabel.c_str(), 1, glow * 0.4f, 0.9f);
		{
			const ImVec2 c = x.P(481, 288);
			IconLeftOf(Role("gauge/boost"), c.x - TextW(LabelFont(blpx), blpx, cfg.boostLabel.c_str()) * 0.5f, c.y,
				x.L(8.0f), cfg.accent, 0.9f);
		}

		{
			const ImVec2 nc = x.P(481, 324);
			const float rN = x.L(21.0f);
			const float v = Clamp01(needleN2o);
			Stroke(dl, ArcPts(nc, rN * 0.97f, kSweepStart, kSweepStart + kSweep, 64), cfg.text, x.L(0.6f), 0.0f, 0.25f);
			if (v > 0.001f) {
				const float flick = 0.5f + 0.5f * std::sin(time * 40.0f);
				const float k = st->nitroActive ? (0.85f + 0.15f * flick) : 1.0f;
				Stroke(dl, ArcPts(nc, rN * 0.97f, kSweepStart, kSweepStart + kSweep * v, 64), cfg.speed, x.L(1.1f), glow * k);
			}
			if (st->nitroRecharging && v < 0.999f) {
				// Recharge comet travelling along the filled part of the arc.
				const float head = v * std::fmod(time * 0.9f, 1.0f);
				const float tail = std::max(0.0f, head - 0.12f);
				Stroke(dl, ArcPts(nc, rN * 0.97f, kSweepStart + kSweep * tail, kSweepStart + kSweep * head, 12), cfg.text, x.L(1.2f), glow);
			}
			const float na = kSweepStart + kSweep * Clamp01(needleN2o);
			Seg(dl, Add(nc, Mul(Dir(na), -rN * 0.2f)), Add(nc, Mul(Dir(na), rN * 0.86f)), cfg.needle, x.L(1.1f), glow);
			dl->AddCircleFilled(nc, x.L(2.2f), Col(cfg.needle));
		}
		const float nlpx = x.L(6.5f);
		TxtGlow(dl, LabelFont(nlpx), nlpx, x.P(481, 334), cfg.text, cfg.n2oLabel.c_str(), 1, glow * 0.4f, 0.9f);
		{
			const ImVec2 c = x.P(481, 334);
			IconLeftOf(Role("gauge/n2o"), c.x - TextW(LabelFont(nlpx), nlpx, cfg.n2oLabel.c_str()) * 0.5f, c.y,
				x.L(8.0f), cfg.text, 0.9f);
		}
	}

	// ---- Minimap -------------------------------------------------------------------------
	void DrawMinimap() {
		const HudState& s = *st;
		const Xf x = MakeXf(origin, size, cfg.minimap, H);
		const ImVec2 c = x.P(88, 280);
		const float Rpx = x.L(cfg.mapRadiusPx);
		const float op = Clamp01(cfg.mapOpacity);
		const float k = Rpx / std::max(5.0f, mapRadius);
		// Heading-up: forward = (sin h, cos h) in (x, z); right = (-cos h, sin h). Screen up = forward.
		const float sh = std::sin(mapHeading), ch = std::cos(mapHeading);
		auto toMap = [&](glm::vec2 w) -> ImVec2 {
			const float dx = w.x - s.carPos.x;
			const float dz = w.y - s.carPos.y;
			const float rx = -ch * dx + sh * dz;
			const float fw = sh * dx + ch * dz;
			return ImVec2(c.x + rx * k, c.y - fw * k);
		};

		// Soft feathered disc: faint stacked rings, no opaque fill or hard frame.
		for (int i = 0; i < 5; ++i) {
			const float t = 1.0f - 0.16f * (float)i;
			dl->AddCircleFilled(c, Rpx * t, ColA(0.02f, 0.03f, 0.05f, 0.05f * op), 56);
		}

		// Clipped line helper (keeps everything inside the round map region).
		auto clipLine = [&](ImVec2 a, ImVec2 b, const glm::vec4& col, float thick, float glow, float alpha) -> void {
			float t0, t1;
			if (!ClipSegCircle(a, b, c, Rpx * 0.98f, t0, t1)) return;
			const ImVec2 d(b.x - a.x, b.y - a.y);
			Seg(dl, Add(a, Mul(d, t0)), Add(a, Mul(d, t1)), col, thick, glow, alpha);
		};

		// Roads: thin light lines, width scaled by the road half-width.
		if (s.roads && !s.roads->Empty()) {
			const auto& nodes = s.roads->Nodes();
			const float reach = std::max(5.0f, mapRadius) * 1.5f;
			for (const auto& e : s.roads->Edges()) {
				if (e.a < 0 || e.b < 0 || e.a >= (int)nodes.size() || e.b >= (int)nodes.size()) continue;
				// Cull edges whose bounds are outside the map disc (large expressway networks).
				const glm::vec2 mn = glm::min(nodes[e.a].pos, nodes[e.b].pos) - glm::vec2(reach);
				const glm::vec2 mx = glm::max(nodes[e.a].pos, nodes[e.b].pos) + glm::vec2(reach);
				if (s.carPos.x < mn.x || s.carPos.x > mx.x || s.carPos.y < mn.y || s.carPos.y > mx.y) continue;
				const float wpx = x.L(cfg.mapRoadPx) * ClampF(e.halfWidth / 7.5f, 0.5f, 2.0f);
				// Stacked roads: other levels are drawn fainter than the car's own level.
				float level = 1.0f;
				if (s.layered) {
					const float y = 0.5f * (nodes[e.a].y + nodes[e.b].y);
					level = std::abs(y - s.carY) > 4.0f ? 0.35f : 1.0f;
				}
				clipLine(toMap(nodes[e.a].pos), toMap(nodes[e.b].pos), glm::vec4(0.82f, 0.90f, 1.0f, 1.0f), wpx, 0.0f, 0.62f * op * level);
			}
		}

		// Route: highlighted polyline in the accent colour.
		for (size_t i = 0; i + 1 < s.route.size(); ++i) {
			clipLine(toMap(s.route[i]), toMap(s.route[i + 1]), cfg.accent, x.L(2.0f), cfg.glow * 0.6f, op);
		}

		// Markers: inside the disc at their position, otherwise clamped to the rim (dimmed).
		for (const auto& m : s.markers) {
			ImVec2 p = toMap(m.pos);
			const ImVec2 u(p.x - c.x, p.y - c.y);
			const float dist = std::sqrt(Dot(u, u));
			float alpha = op;
			const float lim = Rpx - x.L(5.0f);
			bool clamped = false;
			if (dist > lim) {
				if (dist < 1e-4f) continue;
				p = Add(c, Mul(u, lim / dist));
				alpha *= 0.55f;
				clamped = true;
			}
			const float r = x.L(4.2f);
			// Off-map markers pile up on the rim: only the next gate, the finish and events keep their
			// icon there; other checkpoints become a small rim dot (no overlapping icon clusters).
			if (clamped && m.kind == HudMapMarker::Kind::Checkpoint) {
				dl->AddCircleFilled(p, r * 0.45f, Col(cfg.accent, 0.7f * alpha), 10);
				continue;
			}
			switch (m.kind) {
			case HudMapMarker::Kind::Event:
				if (!Icon(Role("event/" + m.label), p, r * 2.4f, cfg.lavender, 0.95f * alpha)) {
					dl->AddCircleFilled(p, r, Col(cfg.lavender, 0.9f * alpha));
					dl->AddCircleFilled(p, r * 0.4f, ColA(1, 1, 1, alpha));
				}
				break;
			case HudMapMarker::Kind::Checkpoint:
				if (!Icon(Role("map/checkpoint"), p, r * 2.6f, cfg.accent, alpha)) dl->AddCircle(p, r, Col(cfg.accent, alpha), 20, x.L(1.1f));
				break;
			case HudMapMarker::Kind::NextCheckpoint:
				dl->AddCircleFilled(p, r * 1.2f, Col(cfg.accent, 0.25f * alpha), 20);
				dl->AddCircle(p, r * 1.2f, Col(cfg.text, alpha), 20, x.L(1.4f));
				Icon(Role("map/next_checkpoint"), p, r * 2.4f, cfg.text, alpha);
				break;
			case HudMapMarker::Kind::Finish:
				if (!Icon(Role("map/finish"), p, r * 2.6f, cfg.text, 0.95f * alpha)) {
					dl->AddRectFilled(Add(p, ImVec2(-r, -r)), Add(p, ImVec2(r, r)), ColA(1, 1, 1, 0.9f * alpha), x.L(1.0f));
					dl->AddRectFilled(Add(p, ImVec2(-r * 0.5f, -r * 0.5f)), Add(p, ImVec2(r * 0.5f, r * 0.5f)), ColA(0, 0, 0, 0.9f * alpha));
				}
				break;
			case HudMapMarker::Kind::Start:
				if (!Icon(Role("map/start"), p, r * 2.6f, glm::vec4(0.45f, 1.0f, 0.55f, 1.0f), 0.95f * alpha)) {
					dl->AddCircleFilled(p, r * 0.8f, ColA(0.45f, 1.0f, 0.55f, 0.95f * alpha));
				}
				break;
			case HudMapMarker::Kind::Place: // parking area / meeting spot: blue square with a parking icon (or "P")
				dl->AddRectFilled(Add(p, ImVec2(-r, -r)), Add(p, ImVec2(r, r)), ColA(0.2f, 0.45f, 0.95f, 0.9f * alpha), x.L(1.2f));
				if (!Icon(Role("map/place"), p, r * 1.6f, glm::vec4(1, 1, 1, 1), alpha)) {
					dl->AddText(Add(p, ImVec2(-r * 0.45f, -r * 0.8f)), ColA(1, 1, 1, alpha), "P");
				}
				break;
			}
		}

		// Player arrow at the centre (heading-up, so it always points up).
		{
			const float sc = x.L(1.0f);
			const ImVec2 tip(c.x, c.y - 6.0f * sc);
			const ImVec2 bl(c.x - 4.2f * sc, c.y + 4.4f * sc);
			const ImVec2 br(c.x + 4.2f * sc, c.y + 4.4f * sc);
			dl->AddTriangleFilled(tip, bl, br, Col(cfg.text, op));
			dl->AddTriangle(tip, bl, br, Col(cfg.accent, op), x.L(0.8f));
		}
	}

	// ---- World-anchored event flag and checkpoint gate -----------------------------------
	void LabelAt(const Xf& x, float ref, ImVec2 p, const glm::vec4& c, const char* text, float alpha, int align) {
		const float px = x.L(ref);
		TxtGlow(dl, LabelFont(px), px, p, c, text, align, cfg.glow * 0.5f, alpha);
	}

	void DrawEventMarker() {
		const HudState& s = *st;
		if (!s.eventMarkerVisible || !s.menuTitle.empty()) return;   // menus own the centre of the screen
		const float d = std::max(0.0f, s.eventMarkerDistance);
		const float f = ClampF(1.15f - d / 700.0f, 0.55f, 1.15f);
		const float alpha = ClampF(1.25f - d / 900.0f, 0.35f, 1.0f);
		Xf x;
		x.base = ImVec2(origin.x + ClampF(s.eventMarkerNdc.x, 0.03f, 0.97f) * size.x,
			origin.y + ClampF(s.eventMarkerNdc.y, 0.03f, 0.97f) * size.y);
		x.s = H / kRefH * cfg.event.scale * f;
		// Flag: pole and pennant.
		Seg(dl, x.P(0, 8), x.P(0, -9), cfg.text, x.L(0.9f), cfg.glow * 0.4f, alpha);
		const ImVec2 f0 = x.P(0, -9), f1 = x.P(11, -6), f2 = x.P(0, -3);
		if (!Icon(Role("event/" + s.eventType), x.P(6, -8), x.L(14.0f), cfg.lavender, alpha)) {
			dl->AddTriangleFilled(f0, f1, f2, Col(cfg.lavender, alpha));
		}
		// Titles.
		LabelAt(x, 16.0f, x.P(0, 17), cfg.lavender, s.eventType.c_str(), alpha, 1);
		std::string sub = s.eventName;
		if (!s.eventDifficulty.empty()) sub += " (" + s.eventDifficulty + ")";
		LabelAt(x, 10.5f, x.P(0, 28), cfg.lavender, sub.c_str(), alpha * 0.9f, 1);
		char dist[32];
		std::snprintf(dist, sizeof(dist), "%d m", (int)std::lround(d));
		LabelAt(x, 8.0f, x.P(0, 39), cfg.text, dist, alpha * 0.8f, 1);
		// Chevron pointing down at the marker.
		Seg(dl, x.P(-4, 46), x.P(0, 50), cfg.text, x.L(0.9f), 0.0f, 0.7f * alpha);
		Seg(dl, x.P(4, 46), x.P(0, 50), cfg.text, x.L(0.9f), 0.0f, 0.7f * alpha);
	}

	void DrawCheckpointMarker() {
		const HudState& s = *st;
		if (!s.checkpointMarkerVisible) return;
		const float d = std::max(0.0f, s.checkpointMarkerDistance);
		const float f = ClampF(1.0f - d / 800.0f, 0.5f, 1.0f);
		const float alpha = ClampF(1.2f - d / 900.0f, 0.3f, 1.0f);
		Xf x;
		x.base = ImVec2(origin.x + ClampF(s.checkpointMarkerNdc.x, 0.03f, 0.97f) * size.x,
			origin.y + ClampF(s.checkpointMarkerNdc.y, 0.03f, 0.97f) * size.y);
		x.s = H / kRefH * f;
		// Gate: two posts and a lintel, with a chevron above.
		Seg(dl, x.P(-9, 6), x.P(-9, -7), cfg.accent, x.L(1.2f), cfg.glow, alpha);
		Seg(dl, x.P(9, 6), x.P(9, -7), cfg.accent, x.L(1.2f), cfg.glow, alpha);
		Seg(dl, x.P(-9, -7), x.P(9, -7), cfg.accent, x.L(1.2f), cfg.glow, alpha);
		if (!Icon(Role("map/checkpoint"), x.P(0, -14), x.L(11.0f), cfg.text, 0.9f * alpha)) {
			Seg(dl, x.P(-5, -13), x.P(0, -17), cfg.text, x.L(0.9f), 0.0f, 0.8f * alpha);
			Seg(dl, x.P(5, -13), x.P(0, -17), cfg.text, x.L(0.9f), 0.0f, 0.8f * alpha);
		}
		char dist[32];
		std::snprintf(dist, sizeof(dist), "%d m", (int)std::lround(d));
		LabelAt(x, 8.5f, x.P(0, 16), cfg.text, dist, alpha * 0.9f, 1);
	}

	// ---- Top-right event / race panel and SPEEDWALL block ---------------------------------
	void DrawPanel() {
		const HudState& s = *st;
		if (s.phase == HudState::Phase::FreeRoam || s.phase == HudState::Phase::Paused) return;
		const Xf x = MakeXf(origin, size, cfg.panel, H);
		const float glow = cfg.glow;

		if (s.phase == HudState::Phase::NearEvent) {
			const float kpx = x.L(9.0f);
			Txt(dl, LabelFont(kpx), kpx, x.P(552, 75.5f), Col(cfg.text), "START EVENT", 2);
			{
				const ImVec2 c = x.P(552, 75.5f);
				IconLeftOf(Role("key/" + s.startKey), c.x - TextW(LabelFont(kpx), kpx, "START EVENT"), c.y, x.L(9.0f), cfg.text, 1.0f);
			}
			// Clickable prompt (pointer region in the same coordinates as the drawing).
			{
				const ImVec2 c = x.P(552, 75.5f);
				const float w = TextW(LabelFont(kpx), kpx, "START EVENT") + x.L(14.0f);
				const ImVec2 a(c.x - w, x.P(0, 68).y), b = x.P(602, 83);
				ui::Pointer::Get().AddRect(ui::HashId(HudRegion::kStart), glm::vec2(a.x, a.y), glm::vec2(b.x, b.y), HudRegion::kZStart, ui::RegionClick);
				if (s.startHovered || s.startPressed) {
					dl->AddRectFilled(a, b, Col(cfg.lavender, s.startPressed ? 0.45f : 0.22f), x.L(2.0f), ImDrawFlags_RoundCornersAll);
					dl->AddRect(a, b, Col(cfg.lavender, 0.9f), x.L(2.0f), ImDrawFlags_RoundCornersAll, x.L(0.7f));
				}
			}
			dl->AddRectFilled(x.P(559, 70), x.P(600, 81), Col(cfg.text, 0.95f), x.L(1.5f), ImDrawFlags_RoundCornersAll);
			const float kkpx = x.L(7.5f);
			Txt(dl, LabelFont(kkpx), kkpx, x.P(579.5f, 75.5f), ColA(0.05f, 0.05f, 0.08f, 1.0f), s.startKey.c_str(), 1);
		}

		// Event title and name.
		const float tpx = x.L(16.0f);
		TxtGlow(dl, LabelFont(tpx), tpx, x.P(547, 103), cfg.lavender, s.eventType.c_str(), 1, glow * 0.5f);
		const float npx = x.L(10.5f);
		TxtGlow(dl, LabelFont(npx), npx, x.P(543, 120), cfg.text, s.eventName.c_str(), 1, glow * 0.3f, 0.92f);

		if (s.phase == HudState::Phase::Racing || s.phase == HudState::Phase::Finished) {
			const std::string t = FormatTime(s.raceTime);
			const float dpx = x.L(18.0f);
			Txt(dl, DigitFont(dpx), dpx, x.P(547, 140), Col(cfg.speed), t.c_str(), 1);
			char cp[32];
			std::snprintf(cp, sizeof(cp), "CP %d/%d", s.checkpoint, s.checkpointCount);
			const float spx = x.L(9.0f);
			Txt(dl, LabelFont(spx), spx, x.P(498, 158), Col(cfg.text), cp, 0);
			std::string last = "LAST ";
			if (!s.splits.empty()) {
				char lb[32];
				std::snprintf(lb, sizeof(lb), "%+.2f", (double)s.splits.back());
				last += lb;
			} else {
				last += "--";
			}
			Txt(dl, LabelFont(spx), spx, x.P(601, 158), Col(cfg.text, 0.85f), last.c_str(), 2);
		}

		if (s.phase == HudState::Phase::NearEvent || s.phase == HudState::Phase::Countdown || s.phase == HudState::Phase::Racing
			|| s.phase == HudState::Phase::Finished) {
			// SPEEDWALL local best block.
			if (s.phase == HudState::Phase::NearEvent || s.phase == HudState::Phase::Countdown) {
				Txt(dl, LabelFont(x.L(7.5f)), x.L(7.5f), x.P(601, 150), ColA(0.85f, 0.88f, 0.95f, 0.85f), "SPEEDWALL", 2);
			}
			dl->AddRectFilled(x.P(496, 164), x.P(604, 181), ColA(0, 0, 0, 0.35f), x.L(2.0f), ImDrawFlags_RoundCornersAll);
			const float bpx = x.L(9.0f);
			Txt(dl, LabelFont(bpx), bpx, x.P(502, 172.5f), Col(cfg.text), s.bestHolder.c_str(), 0);
			const std::string bt = s.bestTime < 0.0f ? std::string("--:--.--") : FormatTime(s.bestTime);
			Txt(dl, LabelFont(bpx), bpx, x.P(600, 172.5f), Col(cfg.text), bt.c_str(), 2);
		}
	}

	// ---- Countdown, results, menus, centre message ---------------------------------------
	void DrawCentre() {
		const HudState& s = *st;
		const Xf x = MakeXf(origin, size, HudPlacement{ "MC", { 0.0f, 0.0f }, 1.0f }, H);
		const float glow = cfg.glow;
		auto cp = [&x](float dx, float dy) { return x.P(320.0f + dx, 180.0f + dy); };

		if (s.phase == HudState::Phase::Countdown) {
			const float c = std::max(0.0f, s.countdown);
			const int n = (int)std::ceil(c);
			char buf[8];
			std::snprintf(buf, sizeof(buf), "%d", n);
			const char* label = n > 0 ? buf : "GO";
			const float frac = c - std::floor(c);
			const float pop = 1.0f + 0.18f * frac * frac;
			const float px = x.L(56.0f) * pop;
			TxtGlow(dl, LabelFont(px), px, cp(0, 0), cfg.text, label, 1, glow);
		}

		if (s.phase == HudState::Phase::Finished) {
			const float hpx = x.L(14.0f);
			TxtGlow(dl, LabelFont(hpx), hpx, cp(0, -84), cfg.lavender, "FINISHED", 1, glow * 0.5f);
			IconLeftOf(Role("result/finished"), cp(0, -84).x - TextW(LabelFont(hpx), hpx, "FINISHED") * 0.5f, cp(0, -84).y,
				x.L(16.0f), cfg.lavender, 1.0f);
			const std::string t = FormatTime(s.lastTime >= 0.0f ? s.lastTime : s.raceTime);
			const float dpx = x.L(22.0f);
			Txt(dl, DigitFont(dpx), dpx, cp(0, -60), Col(cfg.speed), t.c_str(), 1);
			const std::string best = "BEST " + (s.bestTime < 0.0f ? std::string("--:--.--") : FormatTime(s.bestTime));
			const float bpx = x.L(10.0f);
			Txt(dl, LabelFont(bpx), bpx, cp(0, -40), Col(cfg.text, 0.9f), best.c_str(), 1);
			if (s.newRecord) {
				const float p = 0.6f + 0.4f * std::sin(time * 6.0f);
				const float rpx = x.L(11.0f);
				TxtGlow(dl, LabelFont(rpx), rpx, cp(0, -28), cfg.accent, "NEW RECORD", 1, glow, p);
				IconLeftOf(Role("result/record"), cp(0, -28).x - TextW(LabelFont(rpx), rpx, "NEW RECORD") * 0.5f, cp(0, -28).y,
					x.L(12.0f), cfg.accent, p);
			}
		}

		if (!s.menuTitle.empty()) {
			const float top = (s.phase == HudState::Phase::Finished) ? 0.0f : -44.0f;
			const float mpx = x.L(14.0f);
			TxtGlow(dl, LabelFont(mpx), mpx, cp(0, top), cfg.lavender, s.menuTitle.c_str(), 1, glow * 0.4f);
			IconLeftOf(Role("menu/" + s.menuTitle), cp(0, top).x - TextW(LabelFont(mpx), mpx, s.menuTitle.c_str()) * 0.5f,
				cp(0, top).y, x.L(15.0f), cfg.lavender, 0.9f);
			// Modal: the whole target blocks clicks below and takes the wheel (steps the selection).
			ui::Pointer::Get().AddRect(ui::HashId(HudRegion::kModal), glm::vec2(origin.x, origin.y),
				glm::vec2(origin.x + size.x, origin.y + size.y), HudRegion::kZModal, ui::RegionWheel);
			for (size_t i = 0; i < s.menuItems.size(); ++i) {
				const float y = top + 18.0f + (float)i * 15.0f;
				const bool sel = (int)i == s.menuSelected;
				const bool hov = (int)i == s.menuHovered;
				const bool prs = (int)i == s.menuPressed;
				const ImVec2 r0 = cp(-70, y - 7), r1 = cp(70, y + 7);
				ui::Pointer::Get().AddRect(ui::HashId(HudRegion::kMenuItem, (int)i), glm::vec2(r0.x, r0.y), glm::vec2(r1.x, r1.y),
					HudRegion::kZMenuItem, ui::RegionClick);
				if (sel || hov || prs) {
					// Selected: lavender fill; hovered: brighter outline; held: stronger fill, inset.
					const float inset = prs ? x.L(0.8f) : 0.0f;
					const ImVec2 a(r0.x + inset, r0.y + inset), b(r1.x - inset, r1.y - inset);
					dl->AddRectFilled(a, b, Col(cfg.lavender, prs ? 0.55f : (sel ? 0.30f : 0.14f)), x.L(2.0f), ImDrawFlags_RoundCornersAll);
					dl->AddRect(a, b, Col(hov ? cfg.text : cfg.lavender, hov ? 0.95f : 0.8f), x.L(2.0f), ImDrawFlags_RoundCornersAll,
						x.L(hov ? 1.0f : 0.7f));
				}
				const float ipx = x.L(11.0f);
				Txt(dl, LabelFont(ipx), ipx, cp(0, y), sel ? Col(cfg.text) : ColA(0.85f, 0.85f, 0.9f, 0.7f), s.menuItems[i].c_str(), 1);
				IconLeftOf(Role("menu/" + s.menuItems[i]), cp(0, y).x - TextW(LabelFont(ipx), ipx, s.menuItems[i].c_str()) * 0.5f,
					cp(0, y).y, x.L(11.0f), sel ? cfg.text : glm::vec4(0.85f, 0.85f, 0.9f, 1.0f), sel ? 1.0f : 0.7f);
			}
		}

		if (!s.message.empty() && s.messageAlpha > 0.001f) {
			const float px = x.L(14.0f);
			const float ma = ClampF(s.messageAlpha, 0.0f, 1.0f);
			TxtGlow(dl, LabelFont(px), px, cp(0, -78), cfg.text, s.message.c_str(), 1, glow, ma);
			IconLeftOf(MessageRole(s.message), cp(0, -78).x - TextW(LabelFont(px), px, s.message.c_str()) * 0.5f, cp(0, -78).y,
				x.L(15.0f), cfg.redline, ma);
		}
	}

	void DrawHint() {
		const HudState& s = *st;
		if (s.hint.empty()) return;
		const Xf x = MakeXf(origin, size, cfg.hint, H);
		const float px = x.L(9.0f);
		Txt(dl, LabelFont(px), px, x.P(14, 14), ColA(1, 1, 1, 0.7f), s.hint.c_str(), 0, false);
	}
};

// ----------------------------------------------------------------------------------------------

RacingHUD& RacingHUD::Get() {
	static RacingHUD instance;
	return instance;
}

void RacingHUD::LoadIcons() {
	Impl& m = *m_impl;
	if (!m.cfgLoaded) LoadConfig(m_configPath);
	if (m.cfg.iconManifest.empty()) return;
	// Cached inside UiIconAtlas: a repeat call with the same paths is a no-op.
	UiIconAtlas::Get().Load(m.cfg.iconManifest, m.cfg.iconSize, m.cfg.iconAtlas);
}

std::string RacingHUD::IconId(const std::string& role) const { return m_impl->Role(role); }

RacingHUD::RacingHUD() : m_impl(std::make_unique<Impl>()) {}
RacingHUD::~RacingHUD() = default;

const HudConfig& RacingHUD::Config() const { return m_impl->cfg; }
HudConfig& RacingHUD::MutableConfig() { return m_impl->cfg; }
float RacingHUD::RpmMax() const { return m_impl->cfg.rpmMax; }
float RacingHUD::Redline() const { return m_impl->cfg.redlineRpm; }

bool RacingHUD::LoadConfig(const std::string& path) {
	if (!path.empty()) m_configPath = path;
	Impl& m = *m_impl;
	m.cfgLoaded = true;
	m.cfg = HudConfig();
	m.raw = nlohmann::json::object();
	const std::optional<std::string> hudText = AssetIO::ReadText(m_configPath);
	if (!hudText) return false;
	std::stringstream ss(*hudText);
	try {
		const nlohmann::json j = nlohmann::json::parse(ss.str());
		if (!j.is_object()) return false;
		m.raw = j;
		HudConfig& c = m.cfg;
		ReadPlacement(j, "cluster", c.cluster);
		ReadPlacement(j, "minimap", c.minimap);
		ReadPlacement(j, "event", c.event);
		ReadPlacement(j, "panel", c.panel);
		ReadPlacement(j, "hint", c.hint);

		c.iconRoles.clear();
		if (j.contains("icons") && j["icons"].is_object()) {
			const nlohmann::json& ic = j["icons"];
			c.iconManifest = ic.value("manifest", std::string());
			c.iconAtlas = ic.value("atlas", std::string());
			c.iconSize = ic.value("size", 128);
			if (ic.contains("roles") && ic["roles"].is_object()) {
				for (auto it = ic["roles"].begin(); it != ic["roles"].end(); ++it) {
					if (it.value().is_string()) c.iconRoles[it.key()] = it.value().get<std::string>();
				}
			}
		}

		ReadVec(j, "colors", "accent", c.accent);
		ReadVec(j, "colors", "needle", c.needle);
		ReadVec(j, "colors", "redline", c.redline);
		ReadVec(j, "colors", "speed", c.speed);
		ReadVec(j, "colors", "lavender", c.lavender);
		ReadVec(j, "colors", "text", c.text);
		Read(j, "colors", "backing_alpha", c.backingAlpha);
		Read(j, "colors", "glow", c.glow);

		Read(j, "fonts", "label", c.labelFont);
		Read(j, "fonts", "digital", c.digitalFont);
		Read(j, "fonts", "fallback", c.fallbackFont);
		Read(j, "fonts", "text_scale", c.textScale);

		Read(j, "gauges", "rpm_max", c.rpmMax);
		Read(j, "gauges", "redline", c.redlineRpm);
		Read(j, "gauges", "boost_label", c.boostLabel);
		Read(j, "gauges", "boost_max", c.boostMax);
		Read(j, "gauges", "n2o_label", c.n2oLabel);
		Read(j, "gauges", "speed_units", c.speedUnits);

		Read(j, "minimap", "radius_m", c.mapRadiusM);
		Read(j, "minimap", "zoom_with_speed", c.mapZoomWithSpeed);
		Read(j, "minimap", "opacity", c.mapOpacity);
		Read(j, "minimap", "road_px", c.mapRoadPx);
		Read(j, "minimap", "radius_px", c.mapRadiusPx);

		// Sanity clamps so a bad file cannot break drawing.
		c.rpmMax = ClampF(c.rpmMax, 1000.0f, 30000.0f);
		c.redlineRpm = ClampF(c.redlineRpm, 0.0f, c.rpmMax);
		c.boostMax = std::max(0.01f, c.boostMax);
		c.mapRadiusM = std::max(5.0f, c.mapRadiusM);
		c.mapRadiusPx = ClampF(c.mapRadiusPx, 8.0f, 200.0f);
		c.textScale = ClampF(c.textScale, 0.3f, 3.0f);
		return true;
	} catch (...) {
		m.cfg = HudConfig();
		m.raw = nlohmann::json::object();
		return false;
	}
}

bool RacingHUD::SaveConfig(const std::string& path) {
	Impl& m = *m_impl;
	const std::string out = path.empty() ? m_configPath : path;
	const HudConfig& c = m.cfg;
	try {
		// Start from the loaded document so unknown keys survive the round trip.
		nlohmann::json j = m.raw.is_object() ? m.raw : nlohmann::json::object();
		auto placement = [&](const char* name, const HudPlacement& p) {
			j["placement"][name] = { { "anchor", p.anchor }, { "offset", Vec2Json(p.offset) }, { "scale", p.scale } };
		};
		placement("cluster", c.cluster);
		placement("minimap", c.minimap);
		placement("event", c.event);
		placement("panel", c.panel);
		placement("hint", c.hint);
		j["colors"]["accent"] = Vec4Json(c.accent);
		j["colors"]["needle"] = Vec4Json(c.needle);
		j["colors"]["redline"] = Vec4Json(c.redline);
		j["colors"]["speed"] = Vec4Json(c.speed);
		j["colors"]["lavender"] = Vec4Json(c.lavender);
		j["colors"]["text"] = Vec4Json(c.text);
		j["colors"]["backing_alpha"] = c.backingAlpha;
		j["colors"]["glow"] = c.glow;
		j["fonts"]["label"] = c.labelFont;
		j["fonts"]["digital"] = c.digitalFont;
		j["fonts"]["fallback"] = c.fallbackFont;
		j["fonts"]["text_scale"] = c.textScale;
		j["gauges"]["rpm_max"] = c.rpmMax;
		j["gauges"]["redline"] = c.redlineRpm;
		j["gauges"]["boost_label"] = c.boostLabel;
		j["gauges"]["boost_max"] = c.boostMax;
		j["gauges"]["n2o_label"] = c.n2oLabel;
		j["gauges"]["speed_units"] = c.speedUnits;
		j["minimap"]["radius_m"] = c.mapRadiusM;
		j["minimap"]["zoom_with_speed"] = c.mapZoomWithSpeed;
		j["minimap"]["opacity"] = c.mapOpacity;
		j["minimap"]["road_px"] = c.mapRoadPx;
		j["minimap"]["radius_px"] = c.mapRadiusPx;

		// HUD panel saves are authoring writes: loose source tree only (packed builds refuse).
		const std::optional<std::string> logicalOut = std::filesystem::path(out).is_absolute()
			? AssetIO::LogicalFromPhysical(out) : AssetIO::Normalize(out);
		const std::optional<std::filesystem::path> physicalOut = logicalOut ? AssetIO::SourcePath(*logicalOut) : std::nullopt;
		if (!physicalOut) return false;
		if (physicalOut->has_parent_path()) {
			std::error_code ec;
			std::filesystem::create_directories(physicalOut->parent_path(), ec);
		}
		std::ofstream f(*physicalOut, std::ios::binary | std::ios::trunc);
		if (!f) return false;
		f << j.dump(2) << "\n";
		const bool ok = (bool)f;
		if (ok) m.raw = j;
		return ok;
	} catch (...) {
		return false;
	}
}

void RacingHUD::LoadFonts() {
	Impl& m = *m_impl;
	if (!m.cfgLoaded) LoadConfig(m_configPath);
	if (!ImGui::GetCurrentContext()) return;
	ImGuiIO& io = ImGui::GetIO();
	if (!io.Fonts) return;

	auto exists = [](const std::string& p) {
		return !p.empty() && AssetIO::Exists(p);
	};
	// Fixed-size atlas fonts: bake a few sizes; draw code picks the nearest and scales from there.
	auto addFamily = [&](const std::string& primary, const std::vector<float>& sizes,
		std::vector<ImFont*>& outFonts, std::vector<float>& outSizes) {
		outFonts.clear();
		outSizes.clear();
		std::string path = primary;
		if (!exists(path)) path = m.cfg.fallbackFont;
		if (exists(path)) {
			for (float s : sizes) {
				ImFont* f = nullptr;
				if (AssetIO::Buffer bytes = AssetIO::ReadAll(path)) {
					// ImGui frees this atlas-owned copy (IM_FREE); the caller's buffer does not outlive the call.
					void* copy = IM_ALLOC(bytes->size());
					std::memcpy(copy, bytes->data(), bytes->size());
					f = io.Fonts->AddFontFromMemoryTTF(copy, static_cast<int>(bytes->size()), s);
				}
				if (f) {
					outFonts.push_back(f);
					outSizes.push_back(s);
				}
			}
		}
		if (outFonts.empty()) {
			// Last resort: ImGui default font (scaled at draw time).
			ImFont* f = io.Fonts->AddFontDefault();
			outFonts.push_back(f);
			outSizes.push_back(f ? f->FontSize : 13.0f);
		}
	};
	const std::vector<float> labelSizes = { 10.0f, 14.0f, 20.0f, 28.0f, 40.0f, 56.0f, 80.0f };
	const std::vector<float> digitSizes = { 14.0f, 20.0f, 28.0f, 40.0f, 56.0f, 80.0f };
	addFamily(m.cfg.labelFont, labelSizes, m.labelFonts, m.labelSizes);
	addFamily(m.cfg.digitalFont, digitSizes, m.digitFonts, m.digitSizes);
}

void RacingHUD::Draw(ImDrawList* dl, const ImVec2& origin, const ImVec2& size, const HudState& state, float dt) {
	if (!dl || size.x <= 1.0f || size.y <= 1.0f) return;
	Impl& m = *m_impl;
	LoadIcons();
	dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
	try {
		m.dl = dl;
		m.origin = origin;
		m.size = size;
		m.H = size.y;
		m.dt = ClampF(dt, 0.0f, 0.1f);
		m.st = &state;
		m.time += m.dt;
		m.Update();

		m.DrawMinimap();
		m.DrawCluster();
		m.DrawEventMarker();
		m.DrawCheckpointMarker();
		m.DrawPanel();
		m.DrawCentre();
		m.DrawHint();
	} catch (...) {
		// A HUD failure must never escape into the frame.
	}
	dl->PopClipRect();
	m.st = nullptr;
	m.dl = nullptr;
}

void RacingHUD::DrawEditorPanel() {
	Impl& m = *m_impl;
	HudConfig& c = m.cfg;
	if (ImGui::CollapsingHeader("HUD placement")) {
		EditPlacement("cluster", c.cluster);
		EditPlacement("minimap", c.minimap);
		EditPlacement("event", c.event);
		EditPlacement("panel", c.panel);
		EditPlacement("hint", c.hint);
	}
	if (ImGui::CollapsingHeader("HUD colours")) {
		ImGui::ColorEdit4("accent", &c.accent.x);
		ImGui::ColorEdit4("needle", &c.needle.x);
		ImGui::ColorEdit4("redline", &c.redline.x);
		ImGui::ColorEdit4("speed", &c.speed.x);
		ImGui::ColorEdit4("lavender", &c.lavender.x);
		ImGui::ColorEdit4("text", &c.text.x);
		ImGui::SliderFloat("backing alpha", &c.backingAlpha, 0.0f, 1.0f);
		ImGui::SliderFloat("glow", &c.glow, 0.0f, 2.0f);
	}
	if (ImGui::CollapsingHeader("HUD fonts (applied at startup)")) {
		InputStr("label font", c.labelFont);
		InputStr("digital font", c.digitalFont);
		InputStr("fallback font", c.fallbackFont);
		ImGui::SliderFloat("text scale", &c.textScale, 0.3f, 3.0f);
	}
	if (ImGui::CollapsingHeader("HUD gauges")) {
		ImGui::DragFloat("rpm max", &c.rpmMax, 100.0f, 1000.0f, 30000.0f, "%.0f");
		ImGui::DragFloat("redline rpm", &c.redlineRpm, 100.0f, 0.0f, 30000.0f, "%.0f");
		InputStr("boost label", c.boostLabel);
		ImGui::SliderFloat("boost max (load)", &c.boostMax, 0.05f, 4.0f);
		InputStr("n2o label", c.n2oLabel);
		InputStr("speed units", c.speedUnits);
	}
	if (ImGui::CollapsingHeader("HUD minimap")) {
		ImGui::SliderFloat("radius (m)", &c.mapRadiusM, 5.0f, 300.0f);
		ImGui::SliderFloat("zoom with speed (m per km/h)", &c.mapZoomWithSpeed, 0.0f, 0.6f);
		ImGui::SliderFloat("opacity", &c.mapOpacity, 0.0f, 1.0f);
		ImGui::SliderFloat("road width (px)", &c.mapRoadPx, 0.2f, 4.0f);
		ImGui::SliderFloat("size (ref px)", &c.mapRadiusPx, 8.0f, 120.0f);
	}
	if (ImGui::Button("Save hud.json")) {
		m.saveStatus = SaveConfig() ? 1 : -1;
	}
	ImGui::SameLine();
	if (ImGui::Button("Reload")) {
		m.saveStatus = 0;
		LoadConfig(m_configPath);
	}
	ImGui::SameLine();
	if (m.saveStatus == 1) ImGui::TextDisabled("saved");
	else if (m.saveStatus == -1) ImGui::TextColored(ImVec4(1, 0.4f, 0.3f, 1), "save failed");
	else ImGui::TextDisabled("%s", m_configPath.c_str());
}
