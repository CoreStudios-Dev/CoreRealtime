// Copyright (c) 2026, the CoreRealtime contributors
// SPDX-License-Identifier: BSD-3-Clause
//
// Minimal Tracy profiler stub for the CoreRealtime minimal core.
// The full Tracy client submodule was removed as unnecessary; profiling is
// compiled out, so all zones are no-ops. The API surface covers exactly what
// the minimal core sources use (ZoneScoped, ZoneScopedN, FrameMark).

#pragma once

#define ZoneScoped (void)0
#define ZoneScopedN(...) (void)0
#define FrameMark (void)0
