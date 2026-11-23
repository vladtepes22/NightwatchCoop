// Enemy AI bench (test aid; included by party_director.cpp inside its anonymous namespace, before RunTravelSteps).
//
// BB_ENEMY_TRACE=ENTITY + BB_ENEMY_TRACE_FILE=PATH: one CSV row per game frame (the director ticks once per
// SprjFlipper::Update; the native clock steps 1/30 s a frame, so t = frame / 30 is game time, whatever the wall clock)
// while ENTITY's chr is loaded: its newest TimeAct ring animation and the frames since that ring entry was written,
// its physics position and yaw, the local player's position, yaw, animation, the player's centre distance and
// bearing in the enemy's frame (degrees, 0 = in front, + = on its right, as PutLocal's angle), both HPs, the
// sweep spot. Ring layout as TargetAnimTick ([[chr+0x3B0]+0x18]: 10 x 0x10 at +0x20, next write index +0xC0).
//
// BB_ENEMY_SWEEP=dists=D,D..;angles=A,A..;secs=S[;orbit=DEG_PER_S][;start=S0]: once the trace's chr is found and
// S0 s (default 5) after the first host_chr step on ENTITY, the player stands at every (dist, angle) for S s
// (dists outer loop), placed by worldact::StandAtChr (facing the enemy) and placed again when knocked more than
// 2 m off the spot (2 s apart at most). With orbit, a last pass per distance circles him at DEG_PER_S, the player
// put on the circle every frame (the AI reads positions; the player's own animation stays idle). Ends with
// "test: sweep: done"; the smoke run's own --seconds still bound the run.
struct EnemyTrace {
    bool parsed = false, on = false;
    std::uint32_t entity = 0;
    std::FILE* f = nullptr;
    std::uint64_t chr = 0, frame = 0, rows = 0;
    Clock::time_point refresh{};
    std::int32_t ring_idx = -1, anim = -1;
    std::uint64_t anim_frame0 = 0;
    std::int32_t p_ring_idx = -1, p_anim = -1;
    // sweep
    bool sweep = false, sweep_done = false, sweep_armed = false;
    std::vector<float> dists, angles;
    double secs = 20, orbit = 0, start_after = 5;
    std::uint64_t armed_frame = 0, spot_frame0 = 0, last_warp_frame = 0;
    int spot = -1;  // index into dists x (angles + orbit)
    float spot_pos[3] = {0, 0, 0};
};

EnemyTrace& Trace() {
    static EnemyTrace tr;
    return tr;
}

std::vector<float> ParseFloatList(const std::string& s) {
    std::vector<float> v;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(',', i);
        if (j == std::string::npos) j = s.size();
        if (j > i) v.push_back(std::strtof(s.substr(i, j - i).c_str(), nullptr));
        i = j + 1;
    }
    return v;
}

void EnemyTraceParse(EnemyTrace& tr) {
    tr.parsed = true;
    const char* e = std::getenv("BB_ENEMY_TRACE");
    const char* path = std::getenv("BB_ENEMY_TRACE_FILE");
    if (!e || !*e || !path || !*path) return;
    tr.entity = std::uint32_t(std::strtoul(e, nullptr, 10));
    if (!tr.entity) return;
    tr.f = std::fopen(path, "w");
    if (!tr.f) {
        Log("test: trace: cannot open %s", path);
        return;
    }
    std::fprintf(tr.f,
                 "frame,t,spot,anim,anim_frame,ex,ey,ez,eyaw,px,py,pz,pyaw,panim,dist,bearing,php,pmhp,ehp,emhp\n");
    tr.on = true;
    Log("test: trace: entity %u -> %s (one row per game frame)", tr.entity, path);
    const char* sw = std::getenv("BB_ENEMY_SWEEP");
    if (!sw || !*sw) return;
    std::string s = sw;
    std::size_t i = 0;
    while (i < s.size()) {
        std::size_t j = s.find(';', i);
        if (j == std::string::npos) j = s.size();
        const std::string kv = s.substr(i, j - i);
        const std::size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
            if (k == "dists") tr.dists = ParseFloatList(v);
            else if (k == "angles") tr.angles = ParseFloatList(v);
            else if (k == "secs") tr.secs = std::strtod(v.c_str(), nullptr);
            else if (k == "orbit") tr.orbit = std::strtod(v.c_str(), nullptr);
            else if (k == "start") tr.start_after = std::strtod(v.c_str(), nullptr);
        }
        i = j + 1;
    }
    tr.sweep = !tr.dists.empty() && (!tr.angles.empty() || tr.orbit > 0) && tr.secs > 0;
    Log("test: sweep: %s (%zu dists x %zu angles%s, %.0f s each)", tr.sweep ? "on" : "OFF (bad spec)", tr.dists.size(),
        tr.angles.size(), tr.orbit > 0 ? " + orbit" : "", tr.secs);
}

bool ChrPhysRot(std::uint64_t chr, float* yaw) {
    std::uint64_t a = 0;
    float r[4] = {};
    if (!chr || !SafeGet(chr + 0x58, &a) || !a || !SafeGet(a + 8, &a) || !a || !SafeGet(a + 0x3b0, &a) || !a ||
        !SafeGet(a + 0x68, &a) || !a || !SafeRead(a + 0x1d0, r, sizeof r) || !std::isfinite(r[1])) {
        return false;
    }
    *yaw = r[1];
    return true;
}

/// Newest TimeAct ring entry: its index and animation id; false if unreadable.
bool RingNewest(std::uint64_t chr, std::int32_t* idx, std::int32_t* anim) {
    std::uint64_t cm = 0, ta = 0;
    std::int32_t next = -1;
    if (!chr || !SafeGet(chr + 0x3b0, &cm) || !cm || !SafeGet(cm + 0x18, &ta) || !ta || !SafeGet(ta + 0xc0, &next) ||
        next < 0 || next >= 10) {
        return false;
    }
    *idx = (next + 9) % 10;
    return SafeGet(ta + 0x20 + std::uint64_t(*idx) * 0x10, anim);
}

void SweepTick(EnemyTrace& tr, const float e[3], float eyaw, const float p[3]) {
    const std::size_t per = tr.angles.size() + (tr.orbit > 0 ? 1 : 0);
    const std::size_t total = tr.dists.size() * per;
    const std::uint64_t spot_frames = std::uint64_t(tr.secs * 30.0);
    const auto put = [&](float dist, float angle) {
        std::string what;
        worldact::StandAtChr(std::int32_t(tr.entity), dist, angle, &what);
        tr.last_warp_frame = tr.frame;
        const float a = angle * 3.14159265f / 180.0f, fx = -std::sin(eyaw), fz = -std::cos(eyaw);
        tr.spot_pos[0] = e[0] + (fx * std::cos(a) + fz * std::sin(a)) * dist;
        tr.spot_pos[1] = e[1];
        tr.spot_pos[2] = e[2] + (-fx * std::sin(a) + fz * std::cos(a)) * dist;
    };
    if (tr.spot < 0 || tr.frame - tr.spot_frame0 >= spot_frames) {
        ++tr.spot;
        tr.spot_frame0 = tr.frame;
        if (std::size_t(tr.spot) >= total) {
            tr.sweep_done = true;
            Log("test: sweep: done (%zu spots)", total);
            return;
        }
        const float dist = tr.dists[std::size_t(tr.spot) / per];
        const std::size_t ai = std::size_t(tr.spot) % per;
        if (ai < tr.angles.size()) {
            put(dist, tr.angles[ai]);
            Log("test: sweep: spot %d/%zu: %.1f m at %.0f deg (frame %llu)", tr.spot + 1, total, double(dist),
                double(tr.angles[ai]), static_cast<unsigned long long>(tr.frame));
        } else {
            Log("test: sweep: spot %d/%zu: orbit at %.1f m, %.0f deg/s (frame %llu)", tr.spot + 1, total, double(dist),
                tr.orbit, static_cast<unsigned long long>(tr.frame));
        }
        return;
    }
    const float dist = tr.dists[std::size_t(tr.spot) / per];
    const std::size_t ai = std::size_t(tr.spot) % per;
    if (ai >= tr.angles.size()) {
        // orbit: on the circle every frame, the angle from the frames into the spot
        const double ang = std::fmod(double(tr.frame - tr.spot_frame0) / 30.0 * tr.orbit, 360.0) - 180.0;
        put(dist, float(ang));
        return;
    }
    const float dx = p[0] - tr.spot_pos[0], dz = p[2] - tr.spot_pos[2];
    if (dx * dx + dz * dz > 4.0f && tr.frame - tr.last_warp_frame >= 60) {
        put(dist, tr.angles[ai]);
        Log("test: sweep: knocked %.1f m off spot %d: placed again", double(std::sqrt(dx * dx + dz * dz)), tr.spot + 1);
    }
}

void EnemyTraceTick(Test& t) {
    EnemyTrace& tr = Trace();
    if (!tr.parsed) EnemyTraceParse(tr);
    if (!tr.on) return;
    ++tr.frame;
    const auto now = Clock::now();
    if (!tr.chr || now >= tr.refresh) {
        tr.chr = ChrOfEntity(tr.entity);
        tr.refresh = now + std::chrono::seconds(1);
    }
    const std::uint64_t player = LocalPlayer();
    float e[3], p[3], eyaw = 0, pyaw = 0;
    if (!tr.chr || !player || !balance::ChrPosition(tr.chr, e) || !balance::ChrPosition(player, p) ||
        !ChrPhysRot(tr.chr, &eyaw)) {
        return;
    }
    ChrPhysRot(player, &pyaw);
    std::int32_t idx = -1, anim = -1;
    if (RingNewest(tr.chr, &idx, &anim) && idx != tr.ring_idx) {
        tr.ring_idx = idx;
        tr.anim = anim;
        tr.anim_frame0 = tr.frame;
    }
    std::int32_t pidx = -1, panim = -1;
    if (RingNewest(player, &pidx, &panim) && pidx != tr.p_ring_idx) {
        tr.p_ring_idx = pidx;
        tr.p_anim = panim;
    }
    std::int32_t php = -1, pmhp = -1, ehp = -1, emhp = -1;
    ChrHpOf(player, &php, &pmhp);
    ChrHpOf(tr.chr, &ehp, &emhp);
    const float dx = p[0] - e[0], dz = p[2] - e[2];
    const float fx = -std::sin(eyaw), fz = -std::cos(eyaw);  // his forward
    const float rx = fz, rz = -fx;                           // his right (knowledge msb.md handedness)
    const float bearing = std::atan2(dx * rx + dz * rz, dx * fx + dz * fz) * 180.0f / 3.14159265f;
    const float dist = std::sqrt(dx * dx + dz * dz);
    if (tr.sweep && !tr.sweep_done) {
        if (!tr.sweep_armed && t.chr_target == tr.entity) {
            tr.sweep_armed = true;
            tr.armed_frame = tr.frame;
            Log("test: sweep: armed at frame %llu (host_chr on %u); starts in %.0f s",
                static_cast<unsigned long long>(tr.frame), tr.entity, tr.start_after);
        }
        if (tr.sweep_armed && double(tr.frame - tr.armed_frame) >= tr.start_after * 30.0 && ehp > 0) {
            SweepTick(tr, e, eyaw, p);
        }
    }
    std::fprintf(tr.f, "%llu,%.3f,%d,%d,%llu,%.2f,%.2f,%.2f,%.3f,%.2f,%.2f,%.2f,%.3f,%d,%.2f,%.1f,%d,%d,%d,%d\n",
                 static_cast<unsigned long long>(tr.frame), double(tr.frame) / 30.0, tr.spot, tr.anim,
                 static_cast<unsigned long long>(tr.frame - tr.anim_frame0), double(e[0]), double(e[1]), double(e[2]),
                 double(eyaw), double(p[0]), double(p[1]), double(p[2]), double(pyaw), tr.p_anim, double(dist),
                 double(bearing), php, pmhp, ehp, emhp);
    if (++tr.rows % 30 == 0) std::fflush(tr.f);
}
