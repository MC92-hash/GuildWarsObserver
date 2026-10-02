#pragma once
#include <array>

enum class DepthStencilStateType
{
    Enabled,
    EnabledForward,
    Disabled,
    // Depth TEST on, depth WRITE off - the state a transparent pass needs.
    //
    // A blended surface must be occluded by solid geometry in front of it, so the test stays; but
    // it must not STAMP ITSELF into the depth buffer, or the next transparent surface behind it is
    // discarded even where it contributed nothing. Two-sided translucent geometry is the case that
    // needs this: with depth writes on, the near-invisible front face punches a hole in the face
    // behind it.
    EnabledNoWrite
};

class DepthStencilStateManager
{
public:
    DepthStencilStateManager(ID3D11Device* device, ID3D11DeviceContext* device_context)
        : m_device(device)
        , m_deviceContext(device_context)
    {
        // Create depth stencil state for Enabled
        D3D11_DEPTH_STENCIL_DESC dsDescEnabled;
        ZeroMemory(&dsDescEnabled, sizeof(dsDescEnabled));
        dsDescEnabled.DepthEnable = true;
        dsDescEnabled.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsDescEnabled.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;

        ID3D11DepthStencilState* pDSStateEnabled;
        m_device->CreateDepthStencilState(&dsDescEnabled, &pDSStateEnabled);
        m_depthStencilStates[static_cast<size_t>(DepthStencilStateType::Enabled)] = pDSStateEnabled;

        // Create depth stencil state for forward-depth passes (e.g. shadow map)
        D3D11_DEPTH_STENCIL_DESC dsDescEnabledForward;
        ZeroMemory(&dsDescEnabledForward, sizeof(dsDescEnabledForward));
        dsDescEnabledForward.DepthEnable = true;
        dsDescEnabledForward.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsDescEnabledForward.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;

        ID3D11DepthStencilState* pDSStateEnabledForward;
        m_device->CreateDepthStencilState(&dsDescEnabledForward, &pDSStateEnabledForward);
        m_depthStencilStates[static_cast<size_t>(DepthStencilStateType::EnabledForward)] =
          pDSStateEnabledForward;

        // Create depth stencil state for Disabled
        D3D11_DEPTH_STENCIL_DESC dsDescDisabled;
        ZeroMemory(&dsDescDisabled, sizeof(dsDescDisabled));
        dsDescDisabled.DepthEnable = false;
        dsDescDisabled.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDescDisabled.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;

        ID3D11DepthStencilState* pDSStateDisabled;
        m_device->CreateDepthStencilState(&dsDescDisabled, &pDSStateDisabled);
        m_depthStencilStates[static_cast<size_t>(DepthStencilStateType::Disabled)] = pDSStateDisabled;

        // Depth test on, depth write off. Same comparison as Enabled - the depth buffer is
        // cleared to 0 and the near/far are swapped, so GREATER_EQUAL is "in front".
        D3D11_DEPTH_STENCIL_DESC dsDescNoWrite;
        ZeroMemory(&dsDescNoWrite, sizeof(dsDescNoWrite));
        dsDescNoWrite.DepthEnable = true;
        dsDescNoWrite.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dsDescNoWrite.DepthFunc = D3D11_COMPARISON_GREATER_EQUAL;

        ID3D11DepthStencilState* pDSStateNoWrite;
        m_device->CreateDepthStencilState(&dsDescNoWrite, &pDSStateNoWrite);
        m_depthStencilStates[static_cast<size_t>(DepthStencilStateType::EnabledNoWrite)] =
          pDSStateNoWrite;
    }

    ~DepthStencilStateManager()
    {
        for (auto state : m_depthStencilStates)
        {
            if (state)
            {
                state->Release();
            }
        }
    }

    void SetDepthStencilState(DepthStencilStateType type)
    {
        m_deviceContext->OMSetDepthStencilState(m_depthStencilStates[static_cast<size_t>(type)], 1);
    }

private:
    ID3D11Device* m_device;
    ID3D11DeviceContext* m_deviceContext;

    std::array<ID3D11DepthStencilState*, 4> m_depthStencilStates = {nullptr, nullptr, nullptr,
                                                                    nullptr};
};
