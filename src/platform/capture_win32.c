// Copies the current swapchain back buffer into a PNG (used for automated visual checks).
#define COBJMACROS
#include "platform/platform.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include "sokol/sokol_app.h"
#include "sokol/sokol_gfx.h"
#include "stb/stb_image_write.h"

bool plat_capture_png(const char *path) {
    IDXGISwapChain *sc = (IDXGISwapChain *)sapp_d3d11_get_swap_chain();
    sapp_environment env = sapp_get_environment();
    ID3D11Device *dev = (ID3D11Device *)env.d3d11.device;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)env.d3d11.device_context;
    if (!sc || !dev || !ctx) return false;
    ID3D11Texture2D *bb = NULL, *st = NULL;
    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb))) return false;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(bb, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    bool ok = false;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            u8 *px = malloc((size_t)d.Width * d.Height * 4);
            bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            for (UINT y = 0; y < d.Height; y++) {
                const u8 *src = (const u8 *)m.pData + (size_t)y * m.RowPitch;
                u8 *dst = px + (size_t)y * d.Width * 4;
                for (UINT x = 0; x < d.Width; x++) {
                    dst[x * 4 + 0] = src[x * 4 + (bgra ? 2 : 0)];
                    dst[x * 4 + 1] = src[x * 4 + 1];
                    dst[x * 4 + 2] = src[x * 4 + (bgra ? 0 : 2)];
                    dst[x * 4 + 3] = 255;
                }
            }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
            ok = stbi_write_png(path, (int)d.Width, (int)d.Height, 4, px, (int)d.Width * 4) != 0;
            free(px);
        }
        ID3D11Texture2D_Release(st);
    }
    ID3D11Texture2D_Release(bb);
    sg_reset_state_cache();
    return ok;
}

// Reads an RGBA8 render-target image back to the CPU (premultiplied alpha as rendered).
bool plat_read_image(sg_image img, int w, int h, u8 *out) {
    sg_d3d11_image_info info = sg_d3d11_query_image_info(img);
    sapp_environment env = sapp_get_environment();
    ID3D11Device *dev = (ID3D11Device *)env.d3d11.device;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)env.d3d11.device_context;
    ID3D11Texture2D *src = (ID3D11Texture2D *)info.tex2d, *st = NULL;
    if (!src || !dev || !ctx) return false;
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(src, &d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    bool ok = false;
    if (SUCCEEDED(ID3D11Device_CreateTexture2D(dev, &d, NULL, &st))) {
        ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)st, (ID3D11Resource *)src);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)st, 0, D3D11_MAP_READ, 0, &m))) {
            for (int y = 0; y < h && y < (int)d.Height; y++) memcpy(out + (size_t)y * w * 4, (const u8 *)m.pData + (size_t)y * m.RowPitch, (size_t)MIN(w, (int)d.Width) * 4);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)st, 0);
            ok = true;
        }
        ID3D11Texture2D_Release(st);
    }
    sg_reset_state_cache();
    return ok;
}

bool plat_gpu_info(char *name, int cap, double *vram_gb) {
    ID3D11Device *dev = (ID3D11Device *)sg_d3d11_device();
    if (!dev) return false;
    IDXGIDevice *dx = NULL;
    if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dx)) || !dx) return false;
    IDXGIAdapter *ad = NULL;
    bool ok = false;
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dx, &ad)) && ad) {
        DXGI_ADAPTER_DESC d;
        if (SUCCEEDED(IDXGIAdapter_GetDesc(ad, &d))) {
            WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, name, cap, NULL, NULL);
            *vram_gb = (double)d.DedicatedVideoMemory / (1024.0 * 1024.0 * 1024.0);
            ok = true;
        }
        IDXGIAdapter_Release(ad);
    }
    IDXGIDevice_Release(dx);
    return ok;
}
