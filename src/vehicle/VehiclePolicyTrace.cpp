#include "VehiclePolicyTrace.h"

#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <limits>
#include <sstream>

using nlohmann::json;

namespace VehiclePolicyTrace {
	namespace {
		// Minimal JSON object writer. Non-finite numbers are written as null (replayed as NaN).
		class Out {
		public:
			void Key(const char* k) {
				if (!m_first) m_s += ',';
				m_first = false;
				m_s += '"';
				m_s += k;
				m_s += "\":";
			}
			void Num(const char* k, double v) {
				Key(k);
				if (!std::isfinite(v)) { m_s += "null"; return; }
				char buf[40];
				std::snprintf(buf, sizeof buf, "%.9g", v);
				m_s += buf;
			}
			void Int(const char* k, long long v) { Key(k); m_s += std::to_string(v); }
			void Bool(const char* k, bool v) { Key(k); m_s += v ? "true" : "false"; }
			void Raw(const char* k, const std::string& obj) { Key(k); m_s += obj; }
			std::string Take() { return "{" + m_s + "}"; }
		private:
			std::string m_s;
			bool m_first = true;
		};

		void WriteTelemetry(Out& o, const VehicleStepTelemetry& t) {
			o.Num("time", t.time); o.Num("h", t.h);
			o.Num("px", t.pos.x); o.Num("py", t.pos.y);
			o.Num("velx", t.vel.x); o.Num("vely", t.vel.y);
			o.Num("heading", t.heading);
			o.Num("vx", t.vx); o.Num("vy", t.vy); o.Num("yawRate", t.yawRate);
			o.Num("ax", t.ax); o.Num("ay", t.ay);
			o.Num("steerAngle", t.steerAngle);
			o.Num("speed", t.speed); o.Num("speed01", t.speed01); o.Num("beta", t.beta); o.Num("vxAbs", t.vxAbs);
			o.Num("wheelbase", t.wheelbase); o.Num("track", t.track); o.Num("topSpeed", t.topSpeed);
			o.Num("rearSpin", t.rearSpin); o.Num("rpm", t.rpm); o.Num("wheelSpeed", t.wheelSpeed);
			o.Int("gear", t.gear); o.Bool("limiter", t.limiter); o.Bool("airborne", t.airborne); o.Bool("surface", t.surface);
			o.Num("groundPitch", t.groundPitch); o.Num("groundRoll", t.groundRoll);
			o.Int("gearCount", t.gearCount); o.Num("rpmIdle", t.rpmIdle); o.Num("finalDrive", t.finalDrive);
			o.Num("rearWheelRadius", t.rearWheelRadius);
		}

		void WriteInput(Out& o, const VehicleInput& in) {
			o.Num("thr", in.throttle); o.Num("brk", in.brake); o.Num("str", in.steer);
			o.Bool("hb", in.handbrake); o.Bool("nos", in.nitrous);
		}

		void WriteActuation(Out& o, const VehicleActuation& a) {
			o.Int("gear", a.gear); o.Num("steerAngle", a.steerAngle); o.Num("nitroDriveScale", a.nitroDriveScale);
			o.Bool("driveShiftCut", a.driveShiftCut); o.Num("dragScale", a.dragScale); o.Num("rearGripScale", a.rearGripScale);
			o.Num("driveThrottle", a.driveThrottle); o.Num("reverseDrive", a.reverseDrive); o.Num("brakeCommand", a.brakeCommand);
			o.Num("brakeFrontShare", a.brakeFrontShare); o.Num("brakeRearShare", a.brakeRearShare);
			o.Bool("brakeAtStandstill", a.brakeAtStandstill); o.Bool("rearDriveCut", a.rearDriveCut);
			o.Num("rearSpinFloor", a.rearSpinFloor); o.Bool("rearLock", a.rearLock); o.Bool("holdAtRest", a.holdAtRest);
		}

		void WriteDebug(Out& o, const VehiclePolicyDebug& d) {
			o.Num("nitro", d.nitro); o.Num("nitroCooldown", d.nitroCooldown); o.Bool("nitroActive", d.nitroActive);
			o.Num("drift", d.drift); o.Num("driftSign", d.driftSign); o.Num("shiftTimer", d.shiftTimer);
		}

		// Replay side: JSON null is NaN.
		double Get(const json& j, const char* k) {
			if (!j.contains(k)) return std::numeric_limits<double>::quiet_NaN();
			const json& v = j.at(k);
			return v.is_null() ? std::numeric_limits<double>::quiet_NaN() : v.get<double>();
		}
		bool GetBool(const json& j, const char* k) { return j.contains(k) && j.at(k).is_boolean() && j.at(k).get<bool>(); }
		int GetInt(const json& j, const char* k) { return static_cast<int>(Get(j, k)); }
		float GetF(const json& j, const char* k) { return static_cast<float>(Get(j, k)); }

		void ReadTelemetry(const json& j, VehicleStepTelemetry& t) {
			t.time = GetF(j, "time"); t.h = GetF(j, "h");
			t.pos = glm::vec2(GetF(j, "px"), GetF(j, "py"));
			t.vel = glm::vec2(GetF(j, "velx"), GetF(j, "vely"));
			t.heading = GetF(j, "heading");
			t.vx = GetF(j, "vx"); t.vy = GetF(j, "vy"); t.yawRate = GetF(j, "yawRate");
			t.ax = GetF(j, "ax"); t.ay = GetF(j, "ay");
			t.steerAngle = GetF(j, "steerAngle");
			t.speed = GetF(j, "speed"); t.speed01 = GetF(j, "speed01"); t.beta = GetF(j, "beta"); t.vxAbs = GetF(j, "vxAbs");
			t.wheelbase = GetF(j, "wheelbase"); t.track = GetF(j, "track"); t.topSpeed = GetF(j, "topSpeed");
			t.rearSpin = GetF(j, "rearSpin"); t.rpm = GetF(j, "rpm"); t.wheelSpeed = GetF(j, "wheelSpeed");
			t.gear = GetInt(j, "gear"); t.limiter = GetBool(j, "limiter"); t.airborne = GetBool(j, "airborne"); t.surface = GetBool(j, "surface");
			t.groundPitch = GetF(j, "groundPitch"); t.groundRoll = GetF(j, "groundRoll");
			t.gearCount = GetInt(j, "gearCount"); t.rpmIdle = GetF(j, "rpmIdle"); t.finalDrive = GetF(j, "finalDrive");
			t.rearWheelRadius = GetF(j, "rearWheelRadius");
		}

		void ReadInput(const json& j, VehicleInput& in) {
			in.throttle = GetF(j, "thr"); in.brake = GetF(j, "brk"); in.steer = GetF(j, "str");
			in.handbrake = GetBool(j, "hb"); in.nitrous = GetBool(j, "nos");
		}

		void ReadActuation(const json& j, VehicleActuation& a) {
			a.gear = GetInt(j, "gear"); a.steerAngle = GetF(j, "steerAngle"); a.nitroDriveScale = GetF(j, "nitroDriveScale");
			a.driveShiftCut = GetBool(j, "driveShiftCut"); a.dragScale = GetF(j, "dragScale"); a.rearGripScale = GetF(j, "rearGripScale");
			a.driveThrottle = GetF(j, "driveThrottle"); a.reverseDrive = GetF(j, "reverseDrive"); a.brakeCommand = GetF(j, "brakeCommand");
			a.brakeFrontShare = GetF(j, "brakeFrontShare"); a.brakeRearShare = GetF(j, "brakeRearShare");
			a.brakeAtStandstill = GetBool(j, "brakeAtStandstill"); a.rearDriveCut = GetBool(j, "rearDriveCut");
			a.rearSpinFloor = GetF(j, "rearSpinFloor"); a.rearLock = GetBool(j, "rearLock"); a.holdAtRest = GetBool(j, "holdAtRest");
		}

		void ReadDebug(const json& j, VehiclePolicyDebug& d) {
			d.nitro = GetF(j, "nitro"); d.nitroCooldown = GetF(j, "nitroCooldown"); d.nitroActive = GetBool(j, "nitroActive");
			d.drift = GetF(j, "drift"); d.driftSign = GetF(j, "driftSign"); d.shiftTimer = GetF(j, "shiftTimer");
		}

		// Comparison helpers.
		// The policy interface is float, so both sides are compared at float precision (the recorded %.9g text
		// round-trips float exactly but not double).
		void CompareNum(Report& r, const std::string& name, double gotD, double wantD) {
			const double got = static_cast<float>(gotD);
			const double want = static_cast<float>(wantD);
			FieldError& f = r.fields[name];
			++f.count;
			const bool gotNan = std::isnan(got), wantNan = std::isnan(want);
			if (gotNan || wantNan) {
				if (gotNan != wantNan || std::isnan(got) != std::isnan(want)) {
					++f.above;
					f.maxAbs = std::numeric_limits<double>::infinity();
					f.maxRel = std::numeric_limits<double>::infinity();
				}
				return;
			}
			const double d = std::abs(got - want);
			if (d > 0.0) ++f.above;
			if (d > f.maxAbs) f.maxAbs = d;
			if (std::abs(want) > 1e-3) {
				const double rel = d / std::abs(want);
				if (rel > f.maxRel) f.maxRel = rel;
			}
		}

		void CompareExact(Report& r, const char* call, const char* name, double got, double want) {
			if (got == want) return;
			++r.exactMismatches;
			if (std::string(name) == "gear") ++r.gearMismatches;
			if (r.firstMismatch.empty()) {
				std::ostringstream s;
				s << "call " << r.calls << " (" << call << ") " << name << ": replayed " << got << " recorded " << want;
				r.firstMismatch = s.str();
			}
		}

		void CompareBool(Report& r, const char* call, const char* name, bool got, bool want) {
			CompareExact(r, call, name, got ? 1.0 : 0.0, want ? 1.0 : 0.0);
		}

		void CompareAct(Report& r, const char* call, const VehicleActuation& got, const json& want) {
			const std::string p = std::string(call) + ".";
			CompareExact(r, call, "gear", got.gear, GetInt(want, "gear"));
			CompareNum(r, p + "steerAngle", got.steerAngle, Get(want, "steerAngle"));
			CompareNum(r, p + "nitroDriveScale", got.nitroDriveScale, Get(want, "nitroDriveScale"));
			CompareBool(r, call, "driveShiftCut", got.driveShiftCut, GetBool(want, "driveShiftCut"));
			CompareNum(r, p + "dragScale", got.dragScale, Get(want, "dragScale"));
			CompareNum(r, p + "rearGripScale", got.rearGripScale, Get(want, "rearGripScale"));
			CompareNum(r, p + "driveThrottle", got.driveThrottle, Get(want, "driveThrottle"));
			CompareNum(r, p + "reverseDrive", got.reverseDrive, Get(want, "reverseDrive"));
			CompareNum(r, p + "brakeCommand", got.brakeCommand, Get(want, "brakeCommand"));
			CompareNum(r, p + "brakeFrontShare", got.brakeFrontShare, Get(want, "brakeFrontShare"));
			CompareNum(r, p + "brakeRearShare", got.brakeRearShare, Get(want, "brakeRearShare"));
			CompareBool(r, call, "brakeAtStandstill", got.brakeAtStandstill, GetBool(want, "brakeAtStandstill"));
			CompareBool(r, call, "rearDriveCut", got.rearDriveCut, GetBool(want, "rearDriveCut"));
			CompareNum(r, p + "rearSpinFloor", got.rearSpinFloor, Get(want, "rearSpinFloor"));
			CompareBool(r, call, "rearLock", got.rearLock, GetBool(want, "rearLock"));
			CompareBool(r, call, "holdAtRest", got.holdAtRest, GetBool(want, "holdAtRest"));
		}

		void CompareDebug(Report& r, const VehiclePolicyDebug& got, const json& want) {
			CompareNum(r, "debug.nitro", got.nitro, Get(want, "nitro"));
			CompareNum(r, "debug.nitroCooldown", got.nitroCooldown, Get(want, "nitroCooldown"));
			CompareBool(r, "debug", "nitroActive", got.nitroActive, GetBool(want, "nitroActive"));
			CompareNum(r, "debug.drift", got.drift, Get(want, "drift"));
			CompareNum(r, "debug.driftSign", got.driftSign, Get(want, "driftSign"));
			CompareNum(r, "debug.shiftTimer", got.shiftTimer, Get(want, "shiftTimer"));
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Recorder
	// ---------------------------------------------------------------------------------------------
	Recorder::Recorder(IVehiclePolicy& inner, LineSink sink)
		: m_inner(inner), m_sink(std::move(sink)) {}

	void Recorder::Emit(const char* call, const VehicleInput* in, const VehicleStepTelemetry* t, const std::string& body) {
		if (!m_sink) return;
		std::string line = "{\"seq\":" + std::to_string(++m_seq) + ",\"call\":\"" + call + "\"";
		// Sections are appended in a fixed order: in, t, pre/out (in body), dbg.
		if (in) { Out i; WriteInput(i, *in); line += ",\"in\":" + i.Take(); }
		if (t) { Out tt; WriteTelemetry(tt, *t); line += ",\"t\":" + tt.Take(); }
		line += body;
		Out d;
		WriteDebug(d, m_inner.Debug());
		line += ",\"dbg\":" + d.Take() + "}";
		m_sink(line);
	}

	void Recorder::OnTeleport() {
		m_inner.OnTeleport();
		Emit("teleport", nullptr, nullptr, std::string());
	}

	void Recorder::OnRespawn() {
		m_inner.OnRespawn();
		Emit("respawn", nullptr, nullptr, std::string());
	}

	void Recorder::OnHold() {
		m_inner.OnHold();
		Emit("hold", nullptr, nullptr, std::string());
	}

	void Recorder::Control(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleActuation& out) {
		const int gearIn = out.gear;
		m_inner.Control(in, t, out);
		Out pre; pre.Int("gear", gearIn);
		Out o; WriteActuation(o, out);
		Emit("control", &in, &t, ",\"pre\":" + pre.Take() + ",\"out\":" + o.Take());
	}

	void Recorder::YawAssist(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleYawAssist& io) {
		Out pre; pre.Num("yawAcc", io.yawAcc); pre.Num("ax", io.ax);
		m_inner.YawAssist(in, t, io);
		Out o; o.Num("yawAcc", io.yawAcc); o.Num("ax", io.ax);
		Emit("yaw", &in, &t, ",\"pre\":" + pre.Take() + ",\"out\":" + o.Take());
	}

	void Recorder::VelocityCorrection(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleVelocity& io) {
		Out pre; pre.Num("vx", io.vx); pre.Num("vy", io.vy);
		m_inner.VelocityCorrection(in, t, io);
		Out o; o.Num("vx", io.vx); o.Num("vy", io.vy);
		Emit("velocity", &in, &t, ",\"pre\":" + pre.Take() + ",\"out\":" + o.Take());
	}

	void Recorder::PostStep(const VehicleInput& in, const VehicleStepTelemetry& t, VehicleGearRequest& io) {
		Out pre; pre.Int("gear", io.gear);
		m_inner.PostStep(in, t, io);
		Out o; o.Int("gear", io.gear); o.Bool("gearboxShifting", io.gearboxShifting);
		Emit("post_step", &in, &t, ",\"pre\":" + pre.Take() + ",\"out\":" + o.Take());
	}

	// ---------------------------------------------------------------------------------------------
	// Replay
	// ---------------------------------------------------------------------------------------------
	bool Replay(const std::vector<std::string>& lines, IVehiclePolicy& policy, Report& report, std::string* error) {
		for (const std::string& line : lines) {
			if (line.empty()) continue;
			const json rec = json::parse(line, nullptr, false);
			if (!rec.is_object() || !rec.contains("call")) {
				if (error) *error = "malformed trace line " + std::to_string(report.calls + 1);
				return false;
			}
			++report.calls;
			const std::string call = rec.at("call").get<std::string>();
			VehicleStepTelemetry t;
			VehicleInput in;
			if (rec.contains("t")) ReadTelemetry(rec.at("t"), t);
			if (rec.contains("in")) ReadInput(rec.at("in"), in);
			const json pre = rec.contains("pre") ? rec.at("pre") : json::object();
			const json out = rec.contains("out") ? rec.at("out") : json::object();

			if (call == "teleport") policy.OnTeleport();
			else if (call == "respawn") policy.OnRespawn();
			else if (call == "hold") policy.OnHold();
			else if (call == "control") {
				VehicleActuation act;
				act.gear = GetInt(pre, "gear");
				policy.Control(in, t, act);
				CompareAct(report, "control", act, out);
			}
			else if (call == "yaw") {
				VehicleYawAssist io;
				io.yawAcc = GetF(pre, "yawAcc"); io.ax = GetF(pre, "ax");
				policy.YawAssist(in, t, io);
				CompareNum(report, "yaw.yawAcc", io.yawAcc, Get(out, "yawAcc"));
				CompareNum(report, "yaw.ax", io.ax, Get(out, "ax"));
			}
			else if (call == "velocity") {
				VehicleVelocity io;
				io.vx = GetF(pre, "vx"); io.vy = GetF(pre, "vy");
				policy.VelocityCorrection(in, t, io);
				CompareNum(report, "velocity.vx", io.vx, Get(out, "vx"));
				CompareNum(report, "velocity.vy", io.vy, Get(out, "vy"));
			}
			else if (call == "post_step") {
				VehicleGearRequest io;
				io.gear = GetInt(pre, "gear");
				policy.PostStep(in, t, io);
				CompareExact(report, "post_step", "gear", io.gear, GetInt(out, "gear"));
				CompareBool(report, "post_step", "gearboxShifting", io.gearboxShifting, GetBool(out, "gearboxShifting"));
			}
			else {
				if (error) *error = "unknown call '" + call + "' at line " + std::to_string(report.calls);
				return false;
			}
			if (rec.contains("dbg")) CompareDebug(report, policy.Debug(), rec.at("dbg"));
		}
		return true;
	}

	std::string FormatReport(const Report& r) {
		std::ostringstream s;
		char line[256];
		std::snprintf(line, sizeof line, "%-28s %10s %14s %14s %10s\n", "field", "count", "max abs", "max rel", "differ");
		s << line;
		for (const auto& kv : r.fields) {
			std::snprintf(line, sizeof line, "%-28s %10llu %14.6g %14.6g %10llu\n", kv.first.c_str(),
				static_cast<unsigned long long>(kv.second.count), kv.second.maxAbs, kv.second.maxRel,
				static_cast<unsigned long long>(kv.second.above));
			s << line;
		}
		std::snprintf(line, sizeof line, "calls=%llu exact_mismatches=%llu gear_mismatches=%llu\n",
			static_cast<unsigned long long>(r.calls), static_cast<unsigned long long>(r.exactMismatches),
			static_cast<unsigned long long>(r.gearMismatches));
		s << line;
		if (!r.firstMismatch.empty()) s << "first exact mismatch: " << r.firstMismatch << "\n";
		return s.str();
	}
}
