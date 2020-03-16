/*
 * Copyright (c) 2020 Peking University
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met: redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer;
 * redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution;
 * neither the name of the copyright holders nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * Authors: Rock Lee
 */

#include "mem/cache/prefetch_filter/saturated_counter.hh"
#include "mem/cache/prefetch_filter/program_helper.hh"

namespace prefetch_filter {

SaturatedCounter::SaturatedCounter(const uint8_t bits) {
    bits_ = bits > 31 ? 31 : bits;
    maxValue_ = (1 << bits) - 1;
}

SaturatedCounter::SaturatedCounter(const uint8_t bits, const int value) {
    bits_ = bits > 31 ? 31 : bits;
    maxValue_ = (1 << bits) - 1;
    value_ = value;
}

int SaturatedCounter::init(const uint8_t bits) {
    CHECK_RET(bits < 32, "Saturated counter supports up to 31 bits");
    maxValue_ = (1 << bits) - 1;
    return 0;
}

int SaturatedCounter::init(const uint8_t bits, const int value) {
    CHECK_RET(bits < 32, "Saturated counter supports up to 31 bits");
    maxValue_ = (1 << bits) - 1;
    value_ = value;
    return 0;
}
    

} // namespace prefetch_filter
