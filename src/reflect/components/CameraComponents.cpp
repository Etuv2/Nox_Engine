// Camera reflection descriptors. See src/reflect/Reflection.h and the Light descriptor in
// src/reflect/BuiltinComponents.cpp for the pattern.
//
// The camera facet is the node's ECS CameraComponent (SceneNode::GetCameraComponent), carried by camera nodes
// only. A node whose "current" flag is set drives the view in Play mode (src/render/ActiveCamera.h): the render
// camera copies its world transform, fov, near and far plane.

#include "../ComponentBuilders.h"

#include "../../Camera.h"
#include "../../ComponentTypes.h"
#include "../../SceneNode.h"

namespace nox::reflect {

namespace {

// A node is a camera when it is a Camera object or was created as a CAMERA node.
bool IsCameraNode(const SceneNode& node) {
	return node.GetNodeType() == SceneNode::CAMERA || dynamic_cast<const Camera*>(&node) != nullptr;
}

const CameraComponent* CameraOf(const SceneNode& node) {
	return IsCameraNode(node) ? node.GetCameraComponent() : nullptr;
}

CameraComponent* CameraOf(SceneNode& node) {
	return IsCameraNode(node) ? node.GetCameraComponent() : nullptr;
}

void RegisterCamera() {
	ComponentDesc c;
	c.name = "Camera";
	c.category = "Rendering";
	c.tooltip = "Camera parameters of a camera node (ECS camera component).";
	c.order = 20;
	c.has = [](const SceneNode& n) { return CameraOf(n) != nullptr; };
	c.add = [](SceneNode& n) {
		if (!IsCameraNode(n) || n.GetCameraComponent()) return false;
		CameraComponent camera;
		if (const auto* cameraNode = dynamic_cast<const Camera*>(&n)) {
			camera.fov = cameraNode->GetCameraFov();
			camera.nearPlane = cameraNode->GetCameraNearPlane();
			camera.farPlane = cameraNode->GetCameraFarPlane();
		}
		return n.AddCameraComponent(camera) != nullptr;
	};
	c.remove = [](SceneNode& n) {
		if (!CameraOf(n)) return false;
		n.RemoveCameraComponent();
		return true;
	};

	PropertyDesc fov = Ranged(Prop("fov", PropType::Float,
		[](const SceneNode& n) {
			const CameraComponent* cam = CameraOf(n);
			return PropValue(static_cast<double>(cam ? cam->fov : 0.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			CameraComponent* cam = CameraOf(n);
			if (!cam) return false;
			cam->fov = static_cast<float>(std::get<double>(v));
			return true;
		}, PropAngle), 1.0, 179.0, 0.5);
	fov.tooltip = "Vertical field of view in degrees (perspective only)";

	// near and far must stay ordered; the setters reject a value that would invert the frustum.
	PropertyDesc nearPlane = Ranged(Prop("near_plane", PropType::Float,
		[](const SceneNode& n) {
			const CameraComponent* cam = CameraOf(n);
			return PropValue(static_cast<double>(cam ? cam->nearPlane : 0.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			CameraComponent* cam = CameraOf(n);
			const float value = static_cast<float>(std::get<double>(v));
			if (!cam || value >= cam->farPlane) return false;
			cam->nearPlane = value;
			return true;
		}), 0.001, 100000.0, 0.01);

	PropertyDesc farPlane = Ranged(Prop("far_plane", PropType::Float,
		[](const SceneNode& n) {
			const CameraComponent* cam = CameraOf(n);
			return PropValue(static_cast<double>(cam ? cam->farPlane : 0.0f));
		},
		[](SceneNode& n, const PropValue& v) {
			CameraComponent* cam = CameraOf(n);
			const float value = static_cast<float>(std::get<double>(v));
			if (!cam || value <= cam->nearPlane) return false;
			cam->farPlane = value;
			return true;
		}), 0.01, 1000000.0, 1.0);

	PropertyDesc projection = Prop("projection", PropType::Enum,
		[](const SceneNode& n) {
			const CameraComponent* cam = CameraOf(n);
			return PropValue(static_cast<int64_t>(cam ? cam->projectionType : CameraComponent::ProjectionType::Perspective));
		},
		[](SceneNode& n, const PropValue& v) {
			CameraComponent* cam = CameraOf(n);
			if (!cam) return false;
			cam->projectionType = static_cast<CameraComponent::ProjectionType>(std::get<int64_t>(v));
			return true;
		});
	projection.enumNames = { "perspective", "orthographic" };

	PropertyDesc current = Prop("current", PropType::Bool,
		[](const SceneNode& n) {
			const CameraComponent* cam = CameraOf(n);
			return PropValue(cam ? cam->current : false);
		},
		[](SceneNode& n, const PropValue& v) {
			CameraComponent* cam = CameraOf(n);
			if (!cam) return false;
			cam->current = std::get<bool>(v);
			return true;
		});
	current.tooltip = "Drives the view in Play mode (the first node with current set wins). Editor mode ignores it.";

	c.properties = {
		current,
		fov,
		nearPlane,
		farPlane,
		projection,
		Prop("aspect_ratio", PropType::Float,
			[](const SceneNode& n) {
				const CameraComponent* cam = CameraOf(n);
				return PropValue(static_cast<double>(cam ? cam->aspectRatio : 0.0f));
			}, nullptr, PropNoSave),
	};
	Registry::Get().Register(std::move(c));
}

} // namespace

void RegisterCameraComponents() {
	RegisterCamera();
}

} // namespace nox::reflect
