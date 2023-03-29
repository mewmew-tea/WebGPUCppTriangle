// Based and inspired by https://github.com/cwoffenden/hello-webgpu
// and https://github.com/emscripten-core/emscripten/blob/main/test/webgpu_basic_rendering.cpp

#include <webgpu/webgpu_cpp.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgpu.h>

struct Vertex
{
    float pos[2] = { 0.0f, 0.0f };
    float color[3] = { 0.0f, 0.0f, 0.0f };
};

struct UbObject
{
    float pos[2] = { 0.0f, 0.0f };
    float padding[2];  // 構造体サイズが16byteの倍数になるように調整
};


const char shaderCode[] = R"(
    struct VSIn {
        @location(0) Pos : vec2<f32>,
		@location(1) Color : vec3<f32>,
    };
    struct VSOut {
        @builtin(position) Pos: vec4<f32>,
        @location(0) Color: vec3<f32>,
    };
    struct UbObject
    {
        @location(0) Pos : vec2<f32>,
        @location(1) Padding : vec2<f32>,
    };
    @group(0) @binding(0) var<uniform> ubObject : UbObject;

    @vertex
    fn main_v(In: VSIn) -> VSOut {
        var out: VSOut;
        out.Pos = vec4<f32>(In.Pos + ubObject.Pos, 0.0, 1.0);
        out.Color = In.Color;
        return out;
    }
    @fragment
    fn main_f(In : VSOut) -> @location(0) vec4<f32> {
        return vec4<f32>(In.Color, 1.0f);
    }
)";

wgpu::Device device;
wgpu::Queue queue;
wgpu::Buffer readbackBuffer;
wgpu::RenderPipeline pipeline;
wgpu::Surface surface;

wgpu::SwapChain swapChain;
wgpu::TextureView canvasDepthStencilView;
const uint32_t kWidth = 1280;
const uint32_t kHeight = 720;

wgpu::Buffer vertexBuffer; // vertex buffer
wgpu::Buffer indexBuffer; // index buffer
wgpu::Buffer uniformBuffer; // uniform buffer

wgpu::BindGroup bindGroup;

UbObject ubObject;

wgpu::Buffer createBuffer(const void* data, size_t size, wgpu::BufferUsage usage) {
	wgpu::BufferDescriptor desc = {};
	desc.usage = wgpu::BufferUsage::CopyDst | usage;
	desc.size  = size;
	wgpu::Buffer buffer = device.CreateBuffer(&desc);
	queue.WriteBuffer(buffer, 0, data, size);
	return buffer;
}

void initRenderPipelineAndBuffers() {
    //---------------------------------------
    // Shaderのコンパイル、ShaderModule作成
    //---------------------------------------
    wgpu::ShaderModule shaderModule{};

    wgpu::ShaderModuleWGSLDescriptor wgslDesc{};
    wgslDesc.source = shaderCode;

    wgpu::ShaderModuleDescriptor smDesc{};
    smDesc.nextInChain = &wgslDesc;
    shaderModule = device.CreateShaderModule(&smDesc);
    
    //---------------------------------------
    // bufferの作成
    //---------------------------------------
    // 頂点情報（座標と頂点色）
    // 座標系はDirectXと同じY-Up
    Vertex const vertData[] = {
        {  0.8f, -0.8f, 0.0f, 1.0f, 0.0f },     // 右下
        { -0.8f, -0.8f, 0.0f, 0.0f, 1.0f },     // 左下
        { -0.0f,  0.8f, 1.0f, 0.0f, 0.0f },     // 上
    };
    // 頂点インデックス情報
    uint16_t const indxData[] = {
        0, 1, 2
        , 0 // padding。各バッファのサイズは4byteの倍数である必要がある。
    };

    // vertex buffer
    vertexBuffer = createBuffer(vertData, sizeof(vertData), wgpu::BufferUsage::Vertex);
    // index buffer
    indexBuffer = createBuffer(indxData, sizeof(indxData), wgpu::BufferUsage::Index);
    // Uniform buffer
    uniformBuffer = createBuffer(&ubObject, sizeof(ubObject), wgpu::BufferUsage::Uniform);
    
    //---------------------------------------
    // uniformBufferについての
    // group layoutとbind group
    //---------------------------------------
    // bind group layout
    // bind groupの抽象的な情報。render pipelineに覚えてもらうために、ここで作成＆登録する。
    wgpu::BufferBindingLayout buf = {};
    buf.type = wgpu::BufferBindingType::Uniform;
    wgpu::BindGroupLayoutEntry bglEntry = {};
    bglEntry.binding = 0;   // シェーダーソース上の「@binding(0)」に割り当てられる。
    bglEntry.visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    bglEntry.buffer = buf;
    wgpu::BindGroupLayoutDescriptor bglDesc{};
    bglDesc.entryCount = 1;
    bglDesc.entries = &bglEntry;
    wgpu::BindGroupLayout bgl = device.CreateBindGroupLayout(&bglDesc);

    // bind group
    wgpu::BindGroupEntry bgEntry = {};
    bgEntry.binding = 0;
    bgEntry.buffer = uniformBuffer;
    bgEntry.offset = 0;
    bgEntry.size = sizeof(ubObject);

    wgpu::BindGroupDescriptor bgDesc;
    bgDesc.layout = bgl;
    bgDesc.entryCount = 1;
    bgDesc.entries = &bgEntry;
    device.CreateBindGroup(&bgDesc);

    bindGroup = device.CreateBindGroup(&bgDesc);

    //---------------------------------------
    // render pipelineの作成
    //---------------------------------------
    // vertex
    wgpu::VertexAttribute vertAttrs[2] = {};
    vertAttrs[0].format = wgpu::VertexFormat::Float32x2;
    vertAttrs[0].offset = 0;
    vertAttrs[0].shaderLocation = 0;
    vertAttrs[1].format = wgpu::VertexFormat::Float32x3;
    vertAttrs[1].offset = 2 * sizeof(float);
    vertAttrs[1].shaderLocation = 1;
    wgpu::VertexBufferLayout vertexBufferLayout = {};
    vertexBufferLayout.arrayStride = 5 * sizeof(float);
    vertexBufferLayout.attributeCount = 2;
    vertexBufferLayout.attributes = vertAttrs;
    wgpu::VertexState vertexState = {};
    vertexState.module = shaderModule;
    vertexState.entryPoint = "main_v";
    vertexState.bufferCount = 1;
    vertexState.buffers = &vertexBufferLayout;

    // fragment
    wgpu::BlendState blend = {};
    blend.color.operation = wgpu::BlendOperation::Add;
    blend.color.srcFactor = wgpu::BlendFactor::One;
    blend.color.dstFactor = wgpu::BlendFactor::One;
    blend.alpha.operation = wgpu::BlendOperation::Add;
    blend.alpha.srcFactor = wgpu::BlendFactor::One;
    blend.alpha.dstFactor = wgpu::BlendFactor::One;
    wgpu::ColorTargetState colorTargetState{};
    colorTargetState.format = wgpu::TextureFormat::BGRA8Unorm;
    colorTargetState.blend = &blend;
    wgpu::FragmentState fragmentState{};
    fragmentState.module = shaderModule;
    fragmentState.entryPoint = "main_f";
    fragmentState.targetCount = 1;
    fragmentState.targets = &colorTargetState;

    // DepthStencilState
    wgpu::DepthStencilState depthStencilState{};
    depthStencilState.format = wgpu::TextureFormat::Depth32Float;
    depthStencilState.depthWriteEnabled = true;

    // render pipelineの作成
    wgpu::PipelineLayoutDescriptor pllDesc{}; // bindGroupLayoutをまとめたもの
    pllDesc.bindGroupLayoutCount = 1;
    pllDesc.bindGroupLayouts = &bgl;
    wgpu::RenderPipelineDescriptor descriptor{};
    descriptor.layout = device.CreatePipelineLayout(&pllDesc);
    descriptor.vertex = vertexState;
    descriptor.fragment = &fragmentState;
    descriptor.primitive.frontFace = wgpu::FrontFace::CW;   // 表面とするときの頂点の並び順。
                                                            // デフォルトはCCW（反時計回り）
    descriptor.primitive.cullMode = wgpu::CullMode::Back;   // カリングする面（表裏）
    descriptor.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    descriptor.primitive.stripIndexFormat = wgpu::IndexFormat::Undefined;
    descriptor.depthStencil = &depthStencilState;

    pipeline = device.CreateRenderPipeline(&descriptor);
}

void initSwapChain() {
    // HTMLページ内の、Canvas要素からSurfaceを作成
    wgpu::SurfaceDescriptorFromCanvasHTMLSelector canvasDesc{};
    canvasDesc.selector = "#canvas";

    wgpu::SurfaceDescriptor surfDesc{};
    surfDesc.nextInChain = &canvasDesc;
    wgpu::Instance instance{};  // null instance
    surface = instance.CreateSurface(&surfDesc);

    // SwapChain作成
    wgpu::SwapChainDescriptor scDesc{};
    scDesc.usage = wgpu::TextureUsage::RenderAttachment;
    scDesc.format = wgpu::TextureFormat::BGRA8Unorm;
    scDesc.width = kWidth;
    scDesc.height = kHeight;
    scDesc.presentMode = wgpu::PresentMode::Fifo;
    swapChain = device.CreateSwapChain(surface, &scDesc);
}

void initDepthStencilView() {
    // DepthStencilView作成
    wgpu::TextureDescriptor descriptor{};
    descriptor.usage = wgpu::TextureUsage::RenderAttachment;
    descriptor.size = {kWidth, kHeight, 1};
    descriptor.format = wgpu::TextureFormat::Depth32Float;
    canvasDepthStencilView = device.CreateTexture(&descriptor).CreateView();
}


void frame() {
    // バックバッファとDepthStencilViewのクリア
    wgpu::TextureView backbuffer = swapChain.GetCurrentTextureView();
    wgpu::RenderPassColorAttachment attachment{};
    attachment.view = backbuffer;
    attachment.loadOp = wgpu::LoadOp::Clear;
    attachment.storeOp = wgpu::StoreOp::Store;
    attachment.clearValue = {0, 0, 0, 1};

    wgpu::RenderPassDescriptor renderpass{};
    renderpass.colorAttachmentCount = 1;
    renderpass.colorAttachments = &attachment;

    wgpu::RenderPassDepthStencilAttachment depthStencilAttachment = {};
    depthStencilAttachment.view = canvasDepthStencilView;
    depthStencilAttachment.depthClearValue = 0;
    depthStencilAttachment.depthLoadOp = wgpu::LoadOp::Clear;
    depthStencilAttachment.depthStoreOp = wgpu::StoreOp::Store;

    renderpass.depthStencilAttachment = &depthStencilAttachment;

    wgpu::CommandBuffer commands;
    {
        wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
        {
            wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&renderpass);

            // uniform buffer更新＆書き込み
            static float speed = 0.015f;
            ubObject.pos[0] += speed;
            if (ubObject.pos[0] > (1.0f - 0.8f) || ubObject.pos[0] < -(1.0f - 0.8f))
            {
                speed = -speed;
            }
            queue.WriteBuffer(uniformBuffer, 0, &ubObject, sizeof(ubObject));

            pass.SetPipeline(pipeline);
            pass.SetBindGroup(0, bindGroup, 0, 0);
            pass.SetVertexBuffer(0, vertexBuffer, 0, WGPU_WHOLE_SIZE);
            pass.SetIndexBuffer(indexBuffer, wgpu::IndexFormat::Uint16, 0, WGPU_WHOLE_SIZE);
            pass.DrawIndexed(3, 1, 0, 0, 0);
            pass.End();
        }
        commands = encoder.Finish();
    }

    queue.Submit(1, &commands);
}

void run() {
    device.SetUncapturedErrorCallback(
        [](WGPUErrorType errorType, const char* message, void*) {
            printf("%d: %s\n", errorType, message);
        }, nullptr);
    
    queue = device.GetQueue();

    initRenderPipelineAndBuffers();
    initSwapChain();
    initDepthStencilView();

    // メインループ設定
    emscripten_set_main_loop(frame, 0, false);
}

void getDevice(void (*callback)(wgpu::Device)) {
    // Left as null (until supported in Emscripten)
    static const WGPUInstance instance = nullptr;

    wgpuInstanceRequestAdapter(instance, nullptr, [](WGPURequestAdapterStatus status, WGPUAdapter adapter, const char* message, void* userdata) {
        if (message) {
            printf("wgpuInstanceRequestAdapter: %s\n", message);
        }
        if (status == WGPURequestAdapterStatus_Unavailable) {
            printf("WebGPU unavailable; exiting cleanly\n");
            exit(0);
        }
        assert(status == WGPURequestAdapterStatus_Success);

        wgpuAdapterRequestDevice(adapter, nullptr, [](WGPURequestDeviceStatus status, WGPUDevice dev, const char* message, void* userdata) {
            if (message) {
                printf("wgpuAdapterRequestDevice: %s\n", message);
            }
            assert(status == WGPURequestDeviceStatus_Success);

            wgpu::Device device = wgpu::Device::Acquire(dev);
            reinterpret_cast<void (*)(wgpu::Device)>(userdata)(device);
        }, userdata);
    }, reinterpret_cast<void*>(callback));
}

int main() {
    // GetDeviceのあとに残りの処理を行いたいので、コールバックしてもらう必要がある
    getDevice([](wgpu::Device dev) {
        device = dev;
        run();
    });

    return 0;
}
