#pragma once

#include "Mesh.h"
#include "Vertex.h"
#include "PerObjectCB.h"
#include "SkinnedVertexShader.h"  // For BoneMatricesCB and MAX_BONES
#include "Animation/AnimationController.h"
#include "Animation/AnimationClip.h"
#include "Animation/Skeleton.h"
#include <DirectXMath.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <memory>
#include <vector>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

/**
 * @brief Mesh instance with skeletal animation support.
 *
 * Extends the standard MeshInstance to support:
 * - Skinned vertex buffers with bone indices/weights
 * - Bone matrix constant buffer for GPU skinning
 * - Animation controller integration
 */
class AnimatedMeshInstance
{
public:
    /**
     * @brief Creates an animated mesh instance from skinned vertices.
     *
     * @param device D3D11 device.
     * @param vertices Skinned vertex data.
     * @param indices Index data.
     * @param meshId Unique mesh identifier.
     */
    AnimatedMeshInstance(ID3D11Device* device,
                         const std::vector<SkinnedGWVertex>& vertices,
                         const std::vector<uint32_t>& indices,
                         int meshId,
                         // A caller that solves vertex POSITIONS on the CPU every frame needs a
                         // mappable vertex buffer and a copy of the original vertices to go back
                         // to. Everything else stays IMMUTABLE.
                         bool dynamicVertices = false)
        : m_meshId(meshId)
        , m_isSkinned(true)
        , m_dynamicVertices(dynamicVertices)
    {
        if (dynamicVertices)
            m_baseVertices = vertices;
        CreateVertexBuffer(device, vertices);
        CreateIndexBuffer(device, indices);
        CreateBoneMatrixBuffer(device);
    }

    /**
     * @brief Overwrites the vertex positions with externally computed ones.
     *
     * `positions` is one entry per vertex, already in MODEL space - the frame the bone matrices
     * output - so the vertices are pinned to the IDENTITY bone slot (MAX_BONES - 1, which
     * UpdateBoneMatrices always leaves as identity because no skeleton fills the array) and the
     * shader's skinning step becomes a no-op for them.
     */
    bool UpdateVertexPositions(ID3D11DeviceContext* context,
                               const std::vector<XMFLOAT3>& positions)
    {
        if (!m_dynamicVertices || !m_vertexBuffer || !context ||
            positions.size() != m_baseVertices.size())
        {
            return false;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;

        auto* dst = static_cast<SkinnedGWVertex*>(mapped.pData);
        for (size_t i = 0; i < m_baseVertices.size(); i++)
        {
            dst[i] = m_baseVertices[i];
            dst[i].position = positions[i];
            dst[i].boneIndices[0] = static_cast<uint32_t>(MAX_BONES - 1);
            dst[i].boneIndices[1] = dst[i].boneIndices[2] = dst[i].boneIndices[3] =
                static_cast<uint32_t>(MAX_BONES - 1);
            dst[i].boneWeights[0] = 1.0f;
            dst[i].boneWeights[1] = dst[i].boneWeights[2] = dst[i].boneWeights[3] = 0.0f;
        }
        context->Unmap(m_vertexBuffer.Get(), 0);
        return true;
    }

    /**
     * @brief Puts the rigid, bone-skinned vertices back.
     */
    bool RestoreBaseVertices(ID3D11DeviceContext* context)
    {
        if (!m_dynamicVertices || !m_vertexBuffer || !context || m_baseVertices.empty())
            return false;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context->Map(m_vertexBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            return false;
        std::memcpy(mapped.pData, m_baseVertices.data(),
                    sizeof(SkinnedGWVertex) * m_baseVertices.size());
        context->Unmap(m_vertexBuffer.Get(), 0);
        return true;
    }

    bool HasDynamicVertices() const { return m_dynamicVertices; }

    /**
     * @brief Creates an animated mesh instance from a standard mesh.
     *
     * Converts GWVertex to SkinnedGWVertex with default bone (bone 0, weight 1.0).
     *
     * @param device D3D11 device.
     * @param mesh Standard mesh data.
     * @param meshId Unique mesh identifier.
     */
    AnimatedMeshInstance(ID3D11Device* device, const Mesh& mesh, int meshId)
        : m_meshId(meshId)
        , m_mesh(mesh)
        , m_isSkinned(true)
    {
        // Convert to skinned vertices
        std::vector<SkinnedGWVertex> skinnedVertices;
        skinnedVertices.reserve(mesh.vertices.size());
        for (const auto& v : mesh.vertices)
        {
            skinnedVertices.push_back(SkinnedGWVertex(v));
        }

        CreateVertexBuffer(device, skinnedVertices);
        CreateIndexBuffer(device, mesh.indices);
        CreateBoneMatrixBuffer(device);
    }

    ~AnimatedMeshInstance() = default;

    int GetMeshId() const { return m_meshId; }
    bool IsSkinned() const { return m_isSkinned; }

    const PerObjectCB& GetPerObjectData() const { return m_perObjectData; }
    void SetPerObjectData(const PerObjectCB& data) { m_perObjectData = data; }

    /**
     * @brief Sets textures for a specific slot.
     */
    void SetTextures(const std::vector<ID3D11ShaderResourceView*>& textures, int slot)
    {
        if (slot >= 4 || textures.size() >= MAX_NUM_TEX_INDICES)
        {
            return;
        }

        m_textures[slot].clear();
        for (auto* tex : textures)
        {
            m_textures[slot].push_back(tex);
        }
    }

    /**
     * @brief Updates bone matrices from animation controller.
     *
     * Call this each frame after updating the animation controller.
     *
     * @param context D3D11 device context.
     * @param controller Animation controller with evaluated bone matrices.
     */
    void UpdateBoneMatrices(ID3D11DeviceContext* context,
                            const GW::Animation::AnimationController& controller)
    {
        const auto& matrices = controller.GetBoneMatrices();
        UpdateBoneMatrices(context, matrices);
    }

    /**
     * @brief Updates bone matrices directly from a matrix array.
     *
     * @param context D3D11 device context.
     * @param matrices Bone matrices in world space.
     */
    void UpdateBoneMatrices(ID3D11DeviceContext* context,
                            const std::vector<XMFLOAT4X4>& matrices)
    {
        if (!m_boneMatrixBuffer)
        {
            return;
        }

        // Copy matrices to constant buffer (up to MAX_BONES)
        // Transpose each matrix because the shader uses mul(vector, matrix)
        // which expects column-major matrices (same as World matrix in PerObjectCB)
        size_t boneCount = std::min(matrices.size(), MAX_BONES);
        for (size_t i = 0; i < boneCount; i++)
        {
            XMMATRIX mat = XMLoadFloat4x4(&matrices[i]);
            mat = XMMatrixTranspose(mat);
            XMStoreFloat4x4(&m_boneMatricesCB.bones[i], mat);
        }

        // Fill remaining with identity (identity is its own transpose)
        for (size_t i = boneCount; i < MAX_BONES; i++)
        {
            XMStoreFloat4x4(&m_boneMatricesCB.bones[i], XMMatrixIdentity());
        }

        // Update the constant buffer
        context->UpdateSubresource(m_boneMatrixBuffer.Get(), 0, nullptr,
                                   &m_boneMatricesCB, 0, 0);
    }

    /**
     * @brief Resets bone matrices to identity (bind pose).
     */
    void ResetBoneMatrices(ID3D11DeviceContext* context)
    {
        for (size_t i = 0; i < MAX_BONES; i++)
        {
            XMStoreFloat4x4(&m_boneMatricesCB.bones[i], XMMatrixIdentity());
        }
        context->UpdateSubresource(m_boneMatrixBuffer.Get(), 0, nullptr,
                                   &m_boneMatricesCB, 0, 0);
    }

    /**
     * @brief Draws the animated mesh.
     *
     * Binds the bone matrix buffer to slot b3 for the skinned vertex shader.
     *
     * @param context D3D11 device context.
     * @param lodQuality LOD quality level.
     */
    void Draw(ID3D11DeviceContext* context, LODQuality lodQuality = LODQuality::High)
    {
        // Bind vertex buffer
        UINT stride = sizeof(SkinnedGWVertex);
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, m_vertexBuffer.GetAddressOf(), &stride, &offset);

        // Bind index buffer based on LOD
        size_t numIndices = 0;
        switch (lodQuality)
        {
        case LODQuality::High:
            context->IASetIndexBuffer(m_indexBufferHigh.Get(), DXGI_FORMAT_R32_UINT, 0);
            numIndices = m_indexCountHigh;
            break;
        case LODQuality::Medium:
            if (m_indexCountMedium > 0)
            {
                context->IASetIndexBuffer(m_indexBufferMedium.Get(), DXGI_FORMAT_R32_UINT, 0);
                numIndices = m_indexCountMedium;
                break;
            }
            // Fall through
        case LODQuality::Low:
            if (m_indexCountLow > 0)
            {
                context->IASetIndexBuffer(m_indexBufferLow.Get(), DXGI_FORMAT_R32_UINT, 0);
                numIndices = m_indexCountLow;
                break;
            }
            // Fall through
        default:
            context->IASetIndexBuffer(m_indexBufferHigh.Get(), DXGI_FORMAT_R32_UINT, 0);
            numIndices = m_indexCountHigh;
            break;
        }

        // Bind bone matrix buffer to slot 3 (b3 in shader)
        if (m_boneMatrixBuffer)
        {
            context->VSSetConstantBuffers(3, 1, m_boneMatrixBuffer.GetAddressOf());
        }

        // Bind textures — convert ComPtr vector to raw pointer array
        for (int slot = 0; slot < 4; ++slot)
        {
            UINT srv_count = static_cast<UINT>(m_textures[slot].size());
            if (srv_count > 0)
            {
                std::vector<ID3D11ShaderResourceView*> raw_srvs;
                raw_srvs.reserve(srv_count);
                for (const auto& srv : m_textures[slot])
                    raw_srvs.push_back(srv.Get());
                context->PSSetShaderResources(slot, srv_count, raw_srvs.data());
            }
            else
            {
                ID3D11ShaderResourceView* nullSRV = nullptr;
                context->PSSetShaderResources(slot, 1, &nullSRV);
            }
        }

        // Draw
        context->DrawIndexed(static_cast<UINT>(numIndices), 0, 0);
    }

    /**
     * @brief Gets the bone matrix constant buffer for external binding.
     */
    ID3D11Buffer* GetBoneMatrixBuffer() const { return m_boneMatrixBuffer.Get(); }

    /**
     * @brief Gets the current bone matrices.
     */
    const BoneMatricesCB& GetBoneMatricesCB() const { return m_boneMatricesCB; }

private:
    void CreateVertexBuffer(ID3D11Device* device, const std::vector<SkinnedGWVertex>& vertices)
    {
        if (vertices.empty())
        {
            return;
        }

        D3D11_BUFFER_DESC vbDesc = {};
        vbDesc.Usage = m_dynamicVertices ? D3D11_USAGE_DYNAMIC : D3D11_USAGE_IMMUTABLE;
        vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbDesc.CPUAccessFlags = m_dynamicVertices ? D3D11_CPU_ACCESS_WRITE : 0u;
        vbDesc.ByteWidth = static_cast<UINT>(sizeof(SkinnedGWVertex) * vertices.size());
        vbDesc.StructureByteStride = sizeof(SkinnedGWVertex);

        D3D11_SUBRESOURCE_DATA vbData = {};
        vbData.pSysMem = vertices.data();

        device->CreateBuffer(&vbDesc, &vbData, &m_vertexBuffer);
    }

    void CreateIndexBuffer(ID3D11Device* device, const std::vector<uint32_t>& indices)
    {
        if (indices.empty())
        {
            return;
        }

        D3D11_BUFFER_DESC ibDesc = {};
        ibDesc.Usage = D3D11_USAGE_IMMUTABLE;
        ibDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        ibDesc.ByteWidth = static_cast<UINT>(sizeof(uint32_t) * indices.size());
        ibDesc.StructureByteStride = sizeof(uint32_t);

        D3D11_SUBRESOURCE_DATA ibData = {};
        ibData.pSysMem = indices.data();

        device->CreateBuffer(&ibDesc, &ibData, &m_indexBufferHigh);
        m_indexCountHigh = indices.size();

        // For now, use same indices for all LOD levels
        m_indexCountMedium = 0;
        m_indexCountLow = 0;
    }

    void CreateBoneMatrixBuffer(ID3D11Device* device)
    {
        // Initialize to identity matrices
        for (size_t i = 0; i < MAX_BONES; i++)
        {
            XMStoreFloat4x4(&m_boneMatricesCB.bones[i], XMMatrixIdentity());
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.Usage = D3D11_USAGE_DEFAULT;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.ByteWidth = sizeof(BoneMatricesCB);
        cbDesc.CPUAccessFlags = 0;

        D3D11_SUBRESOURCE_DATA cbData = {};
        cbData.pSysMem = &m_boneMatricesCB;

        device->CreateBuffer(&cbDesc, &cbData, &m_boneMatrixBuffer);
    }

private:
    int m_meshId;
    bool m_isSkinned;
    // A mesh whose positions are solved on the CPU owns a mappable vertex buffer and keeps the
    // original bone-skinned vertices so the solver can be switched off without a rebind.
    bool m_dynamicVertices = false;
    std::vector<SkinnedGWVertex> m_baseVertices;
    Mesh m_mesh;  // Keep a copy for reference
    PerObjectCB m_perObjectData;

    ComPtr<ID3D11Buffer> m_vertexBuffer;
    ComPtr<ID3D11Buffer> m_indexBufferHigh;
    ComPtr<ID3D11Buffer> m_indexBufferMedium;
    ComPtr<ID3D11Buffer> m_indexBufferLow;
    size_t m_indexCountHigh = 0;
    size_t m_indexCountMedium = 0;
    size_t m_indexCountLow = 0;

    ComPtr<ID3D11Buffer> m_boneMatrixBuffer;
    BoneMatricesCB m_boneMatricesCB;

    std::array<std::vector<ComPtr<ID3D11ShaderResourceView>>, 4> m_textures;
};
