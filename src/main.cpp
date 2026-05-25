// T2 — Harness de determinismo do Jolt (spike de risco do Projeto NOVA).
//
// Objetivo: derrubar o risco #1 — Jolt é determinístico bit-a-bit entre
// Win-MSVC e Linux-Clang? E o rollback (SaveState/RestoreState) funciona?
//
// Comandos:
//   t2 info
//       Reporta se JPH_CROSS_PLATFORM_DETERMINISTIC está ativo + config. (F0)
//   t2 run    --ticks N [--threads T] [--mode strict|quant] [--quantum Q] [--out FILE] [--bodies B]
//       Roda a sim e escreve "tick  hash" por linha. Rode nos 3 alvos e use
//       `cmp` pra achar a 1a divergência cross-platform. (C1)
//   t2 replay --ticks N [...]
//       Roda 2x in-process e compara tick-a-tick. Mata não-determinismo do
//       PRÓPRIO harness antes de acusar o Jolt. (C2)
//   t2 rollback --ticks N --at K --resim M [...]
//       Roda referência; depois roda até K, SaveState, segue até N, RestoreState,
//       re-sima M ticks e compara com a referência. (C3)
//   t2 cmp FILE_A FILE_B
//       Diff de dois logs de hash; reporta a 1a divergência. (driver cross-platform)
//
// NÃO é código de produção. Cena = pirâmides de caixas + esferas (muitos
// contatos). Veículo/ragdoll e o journal de add/remove (C4) são TODO documentado.

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/StateRecorderImpl.h>

#include "Layers.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>

using namespace JPH;
using namespace JPH::literals;

// ----------------------------- infra do Jolt ------------------------------

static void TraceImpl(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a);
    fprintf(stderr, "\n"); fflush(stderr);
}
#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char *expr, const char *msg, const char *file, uint line) {
    fprintf(stderr, "ASSERT %s:%u: (%s) %s\n", file, line, expr, msg ? msg : "");
    fflush(stderr);
    return false;   // não dispara breakpoint; deixa seguir pra ver o efeito.
}
#endif

// RAII pra registrar/desregistrar o Jolt uma vez por processo.
struct JoltRuntime {
    JoltRuntime() {
        RegisterDefaultAllocator();
        Trace = TraceImpl;
        JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)
        Factory::sInstance = new Factory();
        RegisterTypes();
    }
    ~JoltRuntime() {
        UnregisterTypes();
        delete Factory::sInstance;
        Factory::sInstance = nullptr;
    }
};

// ------------------------------- hashing ----------------------------------

struct Hasher {                                   // FNV-1a 64 bits
    uint64_t h = 1469598103934665603ull;
    void bytes(const void *p, size_t n) {
        const uint8_t *b = static_cast<const uint8_t *>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
    }
    void f32_strict(float v) { bytes(&v, sizeof(v)); }            // bits crus
    void f32_quant(float v, double q) {                            // quantizado
        int64_t k = static_cast<int64_t>(std::llround(static_cast<double>(v) / q));
        bytes(&k, sizeof(k));
    }
};

enum class HashMode { Strict, Quant };

// Hash do estado físico inteiro num tick, em ordem ESTÁVEL de BodyID.
static uint64_t HashState(PhysicsSystem &ps, HashMode mode, double q) {
    BodyIDVector ids;
    ps.GetBodies(ids);
    std::sort(ids.begin(), ids.end(), [](BodyID a, BodyID b) {
        return a.GetIndexAndSequenceNumber() < b.GetIndexAndSequenceNumber();
    });
    const BodyInterface &bi = ps.GetBodyInterfaceNoLock();
    Hasher hh;
    for (BodyID id : ids) {
        // Corpos estáticos não têm MotionProperties → GetLinearVelocity faria
        // deref nulo. Eles não se movem; ficam fora do hash de estado.
        if (bi.GetMotionType(id) == EMotionType::Static) continue;
        RVec3 p = bi.GetCenterOfMassPosition(id);
        Quat  r = bi.GetRotation(id);
        Vec3  lv = bi.GetLinearVelocity(id);
        Vec3  av = bi.GetAngularVelocity(id);
        float comp[13] = {
            (float)p.GetX(), (float)p.GetY(), (float)p.GetZ(),
            r.GetX(), r.GetY(), r.GetZ(), r.GetW(),
            lv.GetX(), lv.GetY(), lv.GetZ(),
            av.GetX(), av.GetY(), av.GetZ(),
        };
        for (float c : comp) {
            if (mode == HashMode::Strict) hh.f32_strict(c); else hh.f32_quant(c, q);
        }
    }
    return hh.h;
}

// ------------------------------- a cena -----------------------------------
// Determinística por construção (posições calculadas, sem RNG). `bodies`
// escala o nº de pirâmides → muitos contatos persistentes + impactos.

static void BuildScene(PhysicsSystem &ps, int complexity) {
    BodyInterface &bi = ps.GetBodyInterface();

    // Chão estático.
    BodyCreationSettings floor(new BoxShape(Vec3(100.0f, 1.0f, 100.0f)),
                               RVec3(0.0_r, -1.0_r, 0.0_r), Quat::sIdentity(),
                               EMotionType::Static, Layers::NON_MOVING);
    bi.CreateAndAddBody(floor, EActivation::DontActivate);

    RefConst<Shape> box = new BoxShape(Vec3(0.5f, 0.5f, 0.5f));

    // `complexity` pirâmides de caixas, espaçadas em grade.
    const int pyramids = std::max(1, complexity);
    const int base = 5;                            // pirâmide de base 5 → 15 caixas
    int grid = (int)std::ceil(std::sqrt((double)pyramids));
    int placed = 0;
    for (int gz = 0; gz < grid && placed < pyramids; ++gz)
    for (int gx = 0; gx < grid && placed < pyramids; ++gx, ++placed) {
        float ox = (gx - grid * 0.5f) * 8.0f;
        float oz = (gz - grid * 0.5f) * 8.0f;
        for (int layer = 0; layer < base; ++layer) {
            int count = base - layer;
            for (int i = 0; i < count; ++i) {
                float x = ox + (i - count * 0.5f) * 1.05f;
                float y = 0.5f + layer * 1.02f;
                BodyCreationSettings b(box, RVec3(x, y, oz), Quat::sIdentity(),
                                       EMotionType::Dynamic, Layers::MOVING);
                bi.CreateAndAddBody(b, EActivation::Activate);
            }
        }
        // Uma esfera caindo sobre cada pirâmide → impacto/desmoronamento.
        BodyCreationSettings s(new SphereShape(0.7f),
                               RVec3(ox + 0.3f, 12.0f, oz), Quat::sIdentity(),
                               EMotionType::Dynamic, Layers::MOVING);
        s.mLinearVelocity = Vec3(0.0f, -5.0f, 0.0f);
        bi.CreateAndAddBody(s, EActivation::Activate);
    }
    ps.OptimizeBroadPhase();
}

// --------------------------- setup da simulação ---------------------------

struct Sim {
    BPLayerInterfaceImpl              bpli;
    ObjectVsBroadPhaseLayerFilterImpl ovbpf;
    ObjectLayerPairFilterImpl         olpf;
    PhysicsSystem                     ps;
    TempAllocatorImpl                 temp{64 * 1024 * 1024};  // escala com os caps abaixo
    JobSystemThreadPool               jobs;

    explicit Sim(int threads)
        : jobs(cMaxPhysicsJobs, cMaxPhysicsBarriers, threads) {
        ps.Init(/*maxBodies*/ 10240, /*numBodyMutexes*/ 0,
                /*maxBodyPairs*/ 65536, /*maxContactConstraints*/ 20480,
                bpli, ovbpf, olpf);
        ps.SetGravity(Vec3(0.0f, -9.81f, 0.0f));
    }
    void Step() {
        // dt fixo + 1 collision step → passo determinístico.
        ps.Update(1.0f / 60.0f, 1, &temp, &jobs);
    }
};

// ------------------------------- comandos ---------------------------------

struct Args {
    int    ticks   = 600;
    int    threads = 4;     // DEVE ser igual nos 3 alvos pro teste valer.
    int    bodies  = 9;
    int    at      = 100;
    int    resim   = 64;
    HashMode mode  = HashMode::Strict;
    double quantum = 1e-3;  // 1 mm / 1e-3 rad — escala de epsilon de reconciliation.
    std::string out;
    std::string fileA, fileB;
};

static bool flag(const char *a, const char *s) { return std::strcmp(a, s) == 0; }

static Args ParseArgs(int argc, char **argv, int start) {
    Args r;
    for (int i = start; i < argc; ++i) {
        auto next = [&](int &dst) { if (i + 1 < argc) dst = std::atoi(argv[++i]); };
        if      (flag(argv[i], "--ticks"))   next(r.ticks);
        else if (flag(argv[i], "--threads")) next(r.threads);
        else if (flag(argv[i], "--bodies"))  next(r.bodies);
        else if (flag(argv[i], "--at"))      next(r.at);
        else if (flag(argv[i], "--resim"))   next(r.resim);
        else if (flag(argv[i], "--quantum") && i + 1 < argc) r.quantum = std::atof(argv[++i]);
        else if (flag(argv[i], "--mode") && i + 1 < argc)
            r.mode = flag(argv[++i], "quant") ? HashMode::Quant : HashMode::Strict;
        else if (flag(argv[i], "--out") && i + 1 < argc) r.out = argv[++i];
    }
    return r;
}

// Roda a sim e devolve hash[t] pra t em [0, ticks]. hash[0] = estado inicial.
static std::vector<uint64_t> Simulate(const Args &a) {
    Sim sim(a.threads);
    BuildScene(sim.ps, a.bodies);
    std::vector<uint64_t> h;
    h.reserve(a.ticks + 1);
    h.push_back(HashState(sim.ps, a.mode, a.quantum));
    for (int t = 0; t < a.ticks; ++t) {
        sim.Step();
        h.push_back(HashState(sim.ps, a.mode, a.quantum));
    }
    return h;
}

static int CmdInfo() {
    printf("=== T2 Jolt determinism harness — info (F0) ===\n");
#ifdef JPH_CROSS_PLATFORM_DETERMINISTIC
    printf("JPH_CROSS_PLATFORM_DETERMINISTIC : ON   (flag ativo no header consumido)\n");
#else
    printf("JPH_CROSS_PLATFORM_DETERMINISTIC : OFF  <<< F0 FALHOU: rebuild com CROSS_PLATFORM_DETERMINISTIC=ON\n");
#endif
#ifdef JPH_DOUBLE_PRECISION
    printf("JPH_DOUBLE_PRECISION             : ON\n");
#else
    printf("JPH_DOUBLE_PRECISION             : OFF (single precision)\n");
#endif
    printf("sizeof(Real)                     : %zu\n", sizeof(Real));
    printf("sizeof(float)                    : %zu\n", sizeof(float));
    printf("FNV-1a sanity (\"NOVA\")           : %016llx\n",
           (unsigned long long)[]{ Hasher h; h.bytes("NOVA", 4); return h.h; }());
    return 0;
}

static int CmdRun(const Args &a) {
    auto h = Simulate(a);
    FILE *f = stdout;
    if (!a.out.empty()) f = std::fopen(a.out.c_str(), "w");
    for (size_t t = 0; t < h.size(); ++t)
        std::fprintf(f, "%zu\t%016llx\n", t, (unsigned long long)h[t]);
    if (f != stdout) std::fclose(f);
    std::fprintf(stderr, "run: %d ticks, %d threads, mode=%s -> final %016llx\n",
                 a.ticks, a.threads, a.mode == HashMode::Strict ? "strict" : "quant",
                 (unsigned long long)h.back());
    return 0;
}

static int CmdReplay(const Args &a) {
    auto h1 = Simulate(a);
    auto h2 = Simulate(a);
    for (size_t t = 0; t < h1.size(); ++t) {
        if (h1[t] != h2[t]) {
            printf("REPLAY FAIL: divergiu no tick %zu (%016llx vs %016llx)\n",
                   t, (unsigned long long)h1[t], (unsigned long long)h2[t]);
            printf("  -> nao-determinismo do PROPRIO harness (RNG/threads/containers). Corrigir antes de C1.\n");
            return 1;
        }
    }
    printf("REPLAY OK: %d ticks identicos em 2 runs same-platform (C2 passou).\n", a.ticks);
    return 0;
}

static int CmdRollback(const Args &a) {
    // Referência: sim limpa de N ticks.
    auto ref = Simulate(a);

    // Caminho com rollback.
    Sim sim(a.threads);
    BuildScene(sim.ps, a.bodies);
    for (int t = 0; t < a.at; ++t) sim.Step();          // até K

    StateRecorderImpl rec;
    sim.ps.SaveState(rec);                               // snapshot em K
    uint64_t hK = HashState(sim.ps, a.mode, a.quantum);
    if (hK != ref[a.at]) {
        printf("ROLLBACK FAIL: estado em K=%d ja diverge da referencia.\n", a.at);
        return 1;
    }

    for (int t = a.at; t < a.ticks; ++t) sim.Step();     // segue até N (descartável)

    rec.Rewind();
    sim.ps.RestoreState(rec);                            // volta pra K
    uint64_t hKr = HashState(sim.ps, a.mode, a.quantum);
    if (hKr != ref[a.at]) {
        printf("ROLLBACK FAIL: RestoreState nao reproduziu o estado em K=%d.\n", a.at);
        return 1;
    }

    // Re-sima M ticks a partir de K e compara com a referência.
    int end = std::min(a.at + a.resim, a.ticks);
    for (int t = a.at; t < end; ++t) {
        sim.Step();
        uint64_t hr = HashState(sim.ps, a.mode, a.quantum);
        if (hr != ref[t + 1]) {
            printf("ROLLBACK FAIL: re-sim divergiu no tick %d (%016llx vs ref %016llx)\n",
                   t + 1, (unsigned long long)hr, (unsigned long long)ref[t + 1]);
            return 1;
        }
    }
    printf("ROLLBACK OK: SaveState@%d + RestoreState + re-sim de %d ticks bateu a referencia (C3 passou).\n",
           a.at, end - a.at);
    printf("  TODO C4: add/remove de body na janela exige journal manual (nao coberto por SaveState).\n");
    return 0;
}

static int CmdCmp(const std::string &fa, const std::string &fb) {
    std::ifstream A(fa), B(fb);
    if (!A || !B) { printf("cmp: nao consegui abrir os arquivos.\n"); return 2; }
    std::string la, lb; size_t line = 0;
    while (std::getline(A, la)) {
        if (!std::getline(B, lb)) { printf("cmp: B terminou antes (linha %zu).\n", line); return 1; }
        if (la != lb) {
            printf("CMP DIVERGE na linha %zu:\n  A: %s\n  B: %s\n", line, la.c_str(), lb.c_str());
            return 1;
        }
        ++line;
    }
    printf("CMP OK: %zu ticks identicos entre os dois alvos.\n", line);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);   // sem buffer: não perder output num abort()
    setvbuf(stderr, nullptr, _IONBF, 0);
    JoltRuntime rt;
    if (argc < 2) {
        printf("uso: t2 <info|run|replay|rollback|cmp> [opcoes]\n");
        return 2;
    }
    std::string cmd = argv[1];
    if (cmd == "info")     return CmdInfo();
    if (cmd == "run")      return CmdRun(ParseArgs(argc, argv, 2));
    if (cmd == "replay")   return CmdReplay(ParseArgs(argc, argv, 2));
    if (cmd == "rollback") return CmdRollback(ParseArgs(argc, argv, 2));
    if (cmd == "cmp") {
        if (argc < 4) { printf("uso: t2 cmp FILE_A FILE_B\n"); return 2; }
        return CmdCmp(argv[2], argv[3]);
    }
    printf("comando desconhecido: %s\n", cmd.c_str());
    return 2;
}
