/*
* Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
*
* SPDX-License-Identifier: BSD-2-Clause
*/

#pragma once

#include <AK/Vector.h>

class CommandBufferBuilder {
public:
    void append_transfer3d_flat(u32 resource, size_t length);
    void append_end_transfers_3d();
    void append_draw_vbo(u32 count);
    void append_gl_clear(float r, float g, float b);
    void append_set_vertex_buffers(u32 stride, u32 offset, u32 resource);
    Vector<u32> const& build() { return m_buffer; }
private:
    Vector<u32> m_buffer;
};
