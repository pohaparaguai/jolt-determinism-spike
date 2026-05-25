# T2 — Harness de determinismo do Jolt

Spike de risco do Projeto NOVA. Valida o **risco técnico #1**: o Jolt é determinístico bit-a-bit entre Windows-MSVC e Linux-Clang? E o rollback (`SaveState`/`RestoreState`) funciona?

Código **descartável** — prova de conceito, não produção. Jolt v5.5.0 via FetchContent com `CROSS_PLATFORM_DETERMINISTIC=ON`.

## Build

Pré-requisitos: CMake ≥ 3.24, compilador C++20, git (FetchContent clona o Jolt).

```bash
# Windows (MSVC / VS 2026)
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target t2
# -> build/Release/t2.exe

# Linux (Clang) — alvo do servidor
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release
cmake --build build --target t2
# -> build/t2
```

> Para o teste cross-platform valer, os 3 alvos (Win-MSVC client, Win-MSVC server, Linux-Clang server) DEVEM usar as MESMAS opções de CMake (ver `CMakeLists.txt`) e o MESMO `--threads`.

## Comandos

| Comando | Testa | O que faz |
|---------|-------|-----------|
| `t2 info` | **F0** | Reporta se `JPH_CROSS_PLATFORM_DETERMINISTIC` está ativo + config |
| `t2 run --ticks N [--threads T] [--bodies B] [--mode strict\|quant] [--quantum Q] [--out FILE]` | **C1** | Roda a sim e escreve `tick<TAB>hash` por linha |
| `t2 replay --ticks N ...` | **C2** | Roda 2× in-process; mata não-determinismo do próprio harness |
| `t2 rollback --ticks N --at K --resim M ...` | **C3** | SaveState@K + RestoreState + re-sim; compara com referência |
| `t2 cmp FILE_A FILE_B` | **C1** | Diff de dois logs; reporta a 1ª divergência |

`--mode strict` = hash dos bits crus dos floats (exige determinismo bit-a-bit).
`--mode quant --quantum 1e-3` = quantiza antes de hashear (testa viabilidade de *soft-reconciliation* por epsilon).

## O teste cross-platform (C1) — o veredito que importa

```bash
# em cada alvo, com params IDÊNTICOS:
t2 run --ticks 1200 --bodies 16 --threads 4 --mode strict --out <alvo>.txt
# depois, num lugar só:
t2 cmp win-msvc.txt linux-clang.txt
```
- `CMP OK` em **strict** nos 3 alvos por ≥10.000 ticks = **determinismo cross-platform confirmado** → predição/rollback viáveis.
- Diverge em strict mas `CMP OK` em **quant** (epsilon estável) = predição viável com soft-reconciliation (PASS condicional).
- Diverge nos dois = acionar Planos B (fixar FP, isolar subsistema, clang-cl no cliente, lockstep). Ver `docs/spikes/T2-determinismo-jolt.md`.

## Estado atual (validado nesta máquina — Win-MSVC 19.50 / VS 2026)

- ✅ **F0**: `JPH_CROSS_PLATFORM_DETERMINISTIC : ON`.
- ✅ **C2**: 600 ticks idênticos em 2 runs same-platform.
- ✅ **C3**: SaveState@100 + RestoreState + re-sim de 64 ticks bateu a referência.
- 📤 **Referência Win-MSVC** gerada em `hashlogs/win-msvc_strict_t1200_th4_b16.txt` (final `b0d311a2fac03884`) — aguardando o build Linux-Clang pra comparar (C1, depende da infra A8).

## TODO (próximas iterações do spike)
- **C4**: add/remove de body na janela de rollback exige *journal manual* (não coberto por `SaveState`). Não implementado.
- Cena mais rica: `VehicleConstraint`, ragdoll/character controller.
- **C5**: medir o custo do flag determinístico (build com/sem, comparar tick time).
- CI matriz Win-MSVC / Linux-Clang comparando os logs automaticamente.
