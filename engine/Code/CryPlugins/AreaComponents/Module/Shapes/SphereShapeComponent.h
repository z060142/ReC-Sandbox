// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

#include "../../Interface/IShapeComponent.h"
#include "ShapeKinds.h"

#include <CrySchematyc/MathTypes.h>
#include <CrySchematyc/Reflection/TypeDesc.h>
#include <CrySchematyc/Env/IEnvRegistrar.h>

#include <vector>

namespace Cry
{
namespace AreaComponents
{

//! The component form of EditorQt's CAreaSphere, minus the area semantics (id, group, priority,
//! fade and links belong to the Area function component, decision 03): a ball of Radius metres
//! centred on Offset in the component's local space.
//!
//! Every contract answer is analytic - there is no point list and no sampling - which is the whole
//! reason a sphere is worth its own kind rather than a many-sided polygon: containment, signed
//! distance, ray intersection and a uniformly distributed interior point are all closed form, and
//! the Area function component's fade distances get an exact hull distance for free.
//!
//! Scale: a sphere cannot represent a non-uniform entity scale (that is an ellipsoid, and every
//! answer above would have to become an ellipsoid solver). It therefore takes the LARGEST axis of
//! the entity scale as a uniform scale and warns once per component instance - O3DE's documented
//! behaviour, see the scale rule in ShapeDisplay.h and research/05 section 1.6.
class CSphereShapeComponent final
	: public IShapeComponent
	, public IShapeComponentEdit
#ifndef RELEASE
	, public IEntityComponentPreviewer
#endif
{
public:
	CSphereShapeComponent() = default;
	virtual ~CSphereShapeComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CSphereShapeComponent>& desc)
	{
		desc.SetGUID("{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96203}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Shape: Sphere");
		desc.SetDescription("A spherical shape. Area functions on the same entity use it as their volume.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		// The reflected interface bases - what makes a cross-DLL GetComponent<IShapeComponent>()
		// and EditorQt's GetAllComponents<IEditorShapeComponent>() find us (IShapeComponent.h).
		desc.AddBase<IShapeComponent>();
		desc.AddBase<IEditorShapeComponent>();

		// One shape per entity (decision 01). One call, every other kind, both directions -
		// see ShapeKinds.h for why this is a list and not twelve hand-written declarations.
		DeclareShapeKindIncompatibilities(desc);

		desc.AddMember(&CSphereShapeComponent::m_radius, 'radi', "Radius", "Radius", "Radius of the sphere in metres", 1.0f);
		desc.AddMember(&CSphereShapeComponent::m_offset, 'offs', "Offset", "Offset", "Translation of the sphere centre in the entity's local space", Vec3(0.0f, 0.0f, 0.0f));
	}

	// IShapeComponent
	virtual EShapeKind GetKind() const override { return EShapeKind::Sphere; }

	virtual void  GetLocalAABB(AABB& out) const override;
	virtual void  GetWorldAABB(AABB& out) const override;
	virtual bool  IsPointInside(const Vec3& world) const override;
	virtual float DistanceToHull(const Vec3& world) const override;
	virtual bool  IntersectRay(const Ray& ray, float& dist) const override;
	virtual bool  GetRandomPointInside(Vec3& out) const override;
	virtual int   GetContour(Vec3* pOutPoints, int maxPoints, bool world) const override;
	virtual void  AddListener(IShapeListener* pListener) override;
	virtual void  RemoveListener(IShapeListener* pListener) override;

	virtual IShapeComponentEdit* GetEditInterface() override { return this; }
	virtual bool                 GetAreaVolume(SShapeAreaVolume& out) const override;
	// ~IShapeComponent

	// IEditorShapeComponent
	virtual bool        GetEditorLocalBounds(AABB& out) const override;
	virtual bool        EditorHitTest(const Ray& worldRay, float tolerance, float& distOut) const override;
	virtual const char* GetEditToolClassName() const override { return "EditTool.AreaShapeEdit"; }
	virtual const char* GetEditToolLabel() const override     { return "Edit Shape"; }
	// ~IEditorShapeComponent

	// IShapeComponentEdit - a sphere has no point list either, so it presents two handles: point 0
	// is the centre (dragging it moves the sphere inside the entity) and point 1 is a radius
	// handle on the local +X axis (dragging it resizes). That is all the generic point tool of the
	// editor plugin needs to know - it never learns that this is a sphere.
	virtual int  GetPointCount() const override { return 2; }
	virtual Vec3 GetPoint(int index) const override;
	virtual void SetPoint(int index, const Vec3& local) override;
	virtual int  InsertPoint(int index, const Vec3& local) override { return -1; } // fixed point set
	virtual void RemovePoint(int index) override                    {}             // fixed point set
	virtual void BeginEdit() override;
	virtual void EndEdit() override;
	//! GetContour() is a full circle, so the polyline the tool draws closes.
	virtual bool IsContourClosed() const override { return true; }
	// ~IShapeComponentEdit

protected:
	// IEntityComponent
	virtual void                    ProcessEvent(const SEntityEvent& event) override;
	virtual Cry::Entity::EventFlags GetEventMask() const override;
	virtual void                    OnShutDown() override;
	// ~IEntityComponent

#ifndef RELEASE
	// IEntityComponentPreviewer
	virtual IEntityComponentPreviewer* GetPreviewer() override { return this; }

	virtual void SerializeProperties(Serialization::IArchive& archive) override {}
	virtual void Render(const IEntity& entity, const IEntityComponent& component, SEntityPreviewContext& context) const override;
	// ~IEntityComponentPreviewer
#endif

private:
	void     NotifyListeners(EShapeChangeReason reason);

	//! World transform of the sphere's own space: the component's world transform with the offset
	//! applied, orthonormalised - the sphere takes its scale through GetWorldRadius() instead, so
	//! that a non-uniform scale can never shear the ball into an ellipsoid.
	Matrix34 GetShapeWorldTM() const;
	//! The centre in world space. The offset goes through the FULL world matrix, scale included,
	//! so that scaling an entity moves its shape's centre the way it moves everything else.
	Vec3     GetWorldCentre() const;
	//! The radius in world space: the authored radius times the largest axis scale of the entity,
	//! with the latched non-uniform-scale warning. See the scale rule in ShapeDisplay.h.
	float    GetWorldRadius() const;

	//! Smallest radius a drag may leave, so that a sphere can never collapse to a point the
	//! editor can no longer pick.
	static constexpr float kMinRadius = 0.01f;
	//! Segments of the circles GetContour() and the previewer draw.
	static constexpr int   kCircleSegments = 32;
	//! Rejection-free uniform sampling needs none, but the cube root does - guard against r == 0.
	static constexpr float kEpsilon = 1e-6f;

	float m_radius = 1.0f;
	Vec3  m_offset = ZERO;

	//! Set between BeginEdit() and EndEdit(): listeners hear one notification per gesture.
	bool m_bInEdit = false;
	bool m_bChangedDuringEdit = false;

	//! Latch for the non-uniform scale warning - told once per component instance, not per frame.
	mutable bool m_bNonUniformScaleWarned = false;

	//! Runtime only, never reflected: non-owning observers that un-register themselves.
	std::vector<IShapeListener*> m_listeners;
};

} // namespace AreaComponents
} // namespace Cry
