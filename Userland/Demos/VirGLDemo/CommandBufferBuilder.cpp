/*
* Copyright (c) 2022, Sahan Fernando <sahan.h.fernando@gmail.com>
*
* SPDX-License-Identifier: BSD-2-Clause
 */

#include "CommandBufferBuilder.h"
#include "VirGLProtocol.h"

static u32 encode_command(u32 length, u32 mid, Protocol::VirGLCommand command) {
    u32 command_value = to_underlying(command);
    return (length << 16) | ((mid & 0xff) << 8) | (command_value & 0xff);
};

void CommandBufferBuilder::append_transfer3d_flat(u32 resource, size_t length) {
    constexpr size_t cmd_len = 13;
    m_buffer.append(encode_command(cmd_len, 0, Protocol::VirGLCommand::TRANSFER3D));
    m_buffer.append(resource); // res_handle
    m_buffer.append(0); // level
    m_buffer.append(242); // usage
    m_buffer.append(0); // stride
    m_buffer.append(0); // layer_stride
    m_buffer.append(0); // x
    m_buffer.append(0); // y
    m_buffer.append(0); // z
    m_buffer.append(length); // width
    m_buffer.append(1); // height
    m_buffer.append(1); // depth
    m_buffer.append(0); // data_offset
    m_buffer.append(1); // direction
}

void CommandBufferBuilder::append_end_transfers_3d() {
    m_buffer.append(encode_command(0, 0, Protocol::VirGLCommand::END_TRANSFERS));
}

void CommandBufferBuilder::append_draw_vbo(u32 count) {
    constexpr size_t cmd_len = 12;
    m_buffer.append(encode_command(cmd_len, 0, Protocol::VirGLCommand::DRAW_VBO));
    m_buffer.append(0); // start
    m_buffer.append(count); // count
    m_buffer.append(AK::to_underlying(Protocol::PipePrimitiveTypes::TRIANGLES)); // mode
    m_buffer.append(0); // indexed
    m_buffer.append(1); // instance_count
    m_buffer.append(0); // index_bias
    m_buffer.append(0); // start_instance
    m_buffer.append(0); // primitive_restart
    m_buffer.append(0); // restart_index
    m_buffer.append(0); // min_index
    m_buffer.append(0xffffffff); // max_index
    m_buffer.append(0); // cso
}

void CommandBufferBuilder::append_gl_clear(float r, float g, float b) {
    m_buffer.append(encode_command(8, 0, Protocol::VirGLCommand::CLEAR));
    m_buffer.append(4);
    m_buffer.append(bit_cast<u32>(r));
    m_buffer.append(bit_cast<u32>(g));
    m_buffer.append(bit_cast<u32>(b));
    m_buffer.append(bit_cast<u32>(1.0f));
    size_t depth_pos = m_buffer.size();
    m_buffer.append(0);
    m_buffer.append(0);
    m_buffer.append(0);
    u64 *depth = (u64*)(&m_buffer[depth_pos]);
    *depth = AK::bit_cast<u64>((double)1.0f);
}

void CommandBufferBuilder::append_set_vertex_buffers(u32 stride, u32 offset, u32 resource) {
    m_buffer.append(encode_command(3, 0, Protocol::VirGLCommand::SET_VERTEX_BUFFERS));
    m_buffer.append(stride);
    m_buffer.append(offset);
    m_buffer.append(resource);
}
