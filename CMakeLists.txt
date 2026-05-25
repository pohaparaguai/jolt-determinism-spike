cmake_minimum_required(VERSION 3.24)
project(t2_jolt_determinism CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---------------------------------------------------------------------------
# F0 — flags de determinismo cross-platform do Jolt.
# CROSS_PLATFORM_DETERMINISTIC=ON é o flag central (desliga FMA, força FP
# previsível). Os 3 alvos (Win-MSVC client, Win-MSVC server, Linux-Clang
# server) DEVEM usar exatamente estas opções, senão o teste é inválido.
# ---------------------------------------------------------------------------
set(CROSS_PLATFORM_DETERMINISTIC ON  CACHE BOOL "" FORCE)
set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
# Casar o runtime MSVC com o nosso target (dinâmico /MD). Jolt usa estático
# por padrão, o que causa LNK2038/LNK2005 no link.
set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
# IPO/LTCG OFF só para acelerar o build do spike; ligar e manter idêntico
# entre plataformas quando for medir determinismo cross-platform de verdade.
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS ON  CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS      OFF CACHE BOOL "" FORCE)
# Não construir extras do Jolt (samples/tests/viewer) — só a lib.
set(TARGET_UNIT_TESTS        OFF CACHE BOOL "" FORCE)
set(TARGET_HELLO_WORLD       OFF CACHE BOOL "" FORCE)
set(TARGET_PERFORMANCE_TEST  OFF CACHE BOOL "" FORCE)
set(TARGET_SAMPLES           OFF CACHE BOOL "" FORCE)
set(TARGET_VIEWER            OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
  JoltPhysics
  GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
  GIT_TAG        v5.5.0
  SOURCE_SUBDIR  Build
)
FetchContent_MakeAvailable(JoltPhysics)

add_executable(t2 src/main.cpp)
target_include_directories(t2 PRIVATE include)
target_link_libraries(t2 PRIVATE Jolt)
