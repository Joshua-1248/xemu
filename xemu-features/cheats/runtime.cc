//
// xemu custom fork - isolated codes runtime hook
//
// Copyright (C) 2026 Joshua-1248
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "runtime.hh"
#include "codes.hh"

void FeatureCodesTick()
{
    g_codes.Tick();
}
