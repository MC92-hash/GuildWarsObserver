#pragma once
#include <DirectXMath.h>
#include "MouseMoveListener.h"

using namespace DirectX;

/**
 * @brief Orbital (arcball) camera for model viewing.
 *
 * Rotates around a target point, supports:
 * - Left-drag: Orbit around target
 * - Right-drag: Pan
 * - Scroll: Zoom in/out
 * - Auto-fit to model bounds
 */
class OrbitalCamera : public MouseMoveListener
{
public:
    OrbitalCamera();
    ~OrbitalCamera() = default;

    // Core functionality
    void Update(float dt);
    void Reset();

    // Target/Focus point
    void SetTarget(const XMFLOAT3& target);
    XMFLOAT3 GetTarget() const { return m_target; }

    // Distance from target
    void SetDistance(float distance);
    float GetDistance() const { return m_distance; }

    // Auto-fit camera to bounding box.
    // `minDistance` is the floor applied to the computed fit distance. The default matches the
    // classic standalone model view, whose models are normalized into a 10000 unit box; a caller
    // that frames geometry in native DAT units passes a smaller floor, otherwise a head sized
    // object ends up a few pixels wide.
    void FitToBounds(const XMFLOAT3& boundsMin, const XMFLOAT3& boundsMax, float minDistance = 4000.0f);

    // Projection setup
    void SetPerspective(float fovY, float aspect, float nearZ, float farZ, bool reverse_z = true);
    void OnViewportChanged(float width, float height);

    // Input handling
    void OnOrbitDrag(float deltaX, float deltaY);  // Left mouse drag
    void OnPanDrag(float deltaX, float deltaY);    // Right mouse drag
    void OnZoom(float delta);                       // Mouse wheel

    // Mouse move listener implementation
    virtual void OnMouseMove(float yaw_angle_radians, float pitch_angle_radians) override;

    // Set current drag mode (called from input handler)
    void SetDragMode(int mode) { m_dragMode = mode; }  // 0=none, 1=orbit, 2=pan
    int GetDragMode() const { return m_dragMode; }

    // Matrix accessors
    XMMATRIX GetView() const;
    XMMATRIX GetProj() const;
    XMFLOAT4X4 GetView4x4() const;
    XMFLOAT4X4 GetProj4x4() const;

    // Camera position (computed from orbit parameters)
    XMFLOAT3 GetPosition() const;
    XMVECTOR GetPositionV() const;

    // Direction vectors
    XMFLOAT3 GetForward() const;
    XMFLOAT3 GetRight() const;
    XMFLOAT3 GetUp() const;

    // Projection parameters
    float GetFovY() const { return m_fovY; }
    float GetAspectRatio() const { return m_aspectRatio; }
    float GetNearZ() const { return m_nearZ; }
    float GetFarZ() const { return m_farZ; }
    bool UsesReverseZ() const { return m_useReverseZ; }

    // Orbit angles
    float GetYaw() const { return m_yaw; }
    float GetPitch() const { return m_pitch; }

    // Point the camera at a specific orbit angle without moving the target or the distance.
    // FitToBounds always lands on the default "30 degrees above" view, so a caller that wants a
    // named view fits first and then sets the angles. Pitch is clamped exactly like an orbit drag.
    void SetOrbitAngles(float yaw, float pitch);

    // Settings
    float m_orbitSensitivity = 0.005f;
    float m_panSensitivity = 1.0f;
    float m_zoomSensitivity = 0.1f;
    float m_minDistance = 10.0f;
    float m_maxDistance = 100000.0f;

private:
    void UpdateViewMatrix();

    // Orbital parameters
    XMFLOAT3 m_target;     // Point we orbit around
    float m_distance;       // Distance from target
    float m_yaw;           // Horizontal rotation (radians)
    float m_pitch;         // Vertical rotation (radians)

    // Projection parameters
    float m_fovY;
    float m_aspectRatio;
    float m_nearZ;
    float m_farZ;
    bool m_useReverseZ;

    // Cached matrices
    XMFLOAT4X4 m_view;
    XMFLOAT4X4 m_proj;
    bool m_viewDirty;

    // Input state
    int m_dragMode;  // 0=none, 1=orbit, 2=pan
};
