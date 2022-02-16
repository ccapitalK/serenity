/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/String.h>
#include <LibMain/Main.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/ioctl_numbers.h>
#include <unistd.h>

#define VIRGL_BIND_DEPTH_STENCIL (1 << 0)
#define VIRGL_BIND_RENDER_TARGET (1 << 1)
#define VIRGL_BIND_SAMPLER_VIEW  (1 << 3)
#define VIRGL_BIND_VERTEX_BUFFER (1 << 4)
#define VIRGL_BIND_INDEX_BUFFER  (1 << 5)
#define VIRGL_BIND_CONSTANT_BUFFER (1 << 6)
#define VIRGL_BIND_DISPLAY_TARGET (1 << 7)
#define VIRGL_BIND_COMMAND_ARGS  (1 << 8)
#define VIRGL_BIND_STREAM_OUTPUT (1 << 11)
#define VIRGL_BIND_SHADER_BUFFER (1 << 14)
#define VIRGL_BIND_QUERY_BUFFER  (1 << 15)
#define VIRGL_BIND_CURSOR        (1 << 16)
#define VIRGL_BIND_CUSTOM        (1 << 17)
#define VIRGL_BIND_SCANOUT       (1 << 18)

namespace Protocol {

// Specification equivalent: enum virtio_gpu_ctrl_type
enum class CommandType : u32 {
    /* 2d commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
    VIRTIO_GPU_CMD_GET_CAPSET,
    VIRTIO_GPU_CMD_GET_EDID,

    /* 3d commands */
    VIRTIO_GPU_CMD_CTX_CREATE = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D,
    VIRTIO_GPU_CMD_SUBMIT_3D,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB,

    /* cursor commands */
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,

    /* success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID,

    /* error responses */
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

enum class ObjectType : u32 {
    NONE,
    BLEND,
    RASTERIZER,
    DSA,
    SHADER,
    VERTEX_ELEMENTS,
    SAMPLER_VIEW,
    SAMPLER_STATE,
    SURFACE,
    QUERY,
    STREAMOUT_TARGET,
    MSAA_SURFACE,
    MAX_OBJECTS,
};

enum class PipeTextureTarget : u32 {
    BUFFER = 0,
    TEXTURE_1D,
    TEXTURE_2D,
    TEXTURE_3D,
    TEXTURE_CUBE,
    TEXTURE_RECT,
    TEXTURE_1D_ARRAY,
    TEXTURE_2D_ARRAY,
    TEXTURE_CUBE_ARRAY,
    MAX
};

enum class PipePrimitiveTypes : u32 {
    POINTS = 0,
    LINES,
    LINE_LOOP,
    LINE_STRIP,
    TRIANGLES,
    TRIANGLE_STRIP,
    TRIANGLE_FAN,
    QUADS,
    QUAD_STRIP,
    POLYGON,
    LINES_ADJACENCY,
    LINE_STRIP_ADJACENCY,
    TRIANGLES_ADJACENCY,
    TRIANGLE_STRIP_ADJACENCY,
    PATCHES,
    MAX
};

}

namespace Gallium {

enum class PipeTextureTarget: u32 {
    BUFFER,
    TEXTURE_1D,
    TEXTURE_2D,
    TEXTURE_3D,
    TEXTURE_CUBE,
    TEXTURE_RECT,
    TEXTURE_1D_ARRAY,
    TEXTURE_2D_ARRAY,
    TEXTURE_CUBE_ARRAY,
    MAX_TEXTURE_TYPES,
};

enum class ShaderType : u32 {
    SHADER_VERTEX = 0,
    SHADER_FRAGMENT,
    SHADER_GEOMETRY,
    SHADER_TESS_CTRL,
    SHADER_TESS_EVAL,
    SHADER_COMPUTE,
    SHADER_TYPES
};

}

struct Resource3DSpecification {
    Gallium::PipeTextureTarget target;
    u32 format;
    u32 bind;
    u32 width;
    u32 height;
    u32 depth;
    u32 array_size;
    u32 last_level;
    u32 nr_samples;
    u32 flags;
};

int gpu_fd;
u32 vbo_resource_id;

static void init() {
    // Open the device
    gpu_fd = open("/dev/gpu0", O_RDWR);
    VERIFY(gpu_fd >= 0);
    // Create a vertex elements resource
    // VirGL3DResourceSpec vbo_spec {
    //     .target = AK::to_underlying(Gallium::PipeTextureTarget::BUFFER), // pipe_texture_target
    //     .format = 45, // pipe_to_virgl_format
    //     .bind = VIRGL_BIND_VERTEX_BUFFER,
    //     .width = PAGE_SIZE,
    //     .height = 1,
    //     .depth = 1,
    //     .array_size = 1,
    //     .last_level = 0,
    //     .nr_samples = 0,
    //     .flags = 0,
    //     .created_resource_id = 0,
    // };
    // VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_CREATE_RESOURCE, &vbo_spec) >= 0);
    // vbo_resource_id = vbo_spec.created_resource_id;
    // Do kernel space setup (disable writes from display server)
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SETUP_DEMO) >= 0);
}

static void draw_frame() {
    dbgln("Going to draw");
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_SUBMIT_CMD) >= 0);
    VERIFY(ioctl(gpu_fd, VIRGL_IOCTL_FLUSH_DISPLAY) >= 0);
}

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    (void) arguments;
    outln("Hello world");
    init();
    while (true) {
        draw_frame();
        usleep(200000);
    }
    return {0};
}
