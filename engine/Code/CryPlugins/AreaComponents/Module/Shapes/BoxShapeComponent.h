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

//! Stage 0 placeholder: an axis-aligned box in the component's local space, offset by m_offset.
//! It exists to prove the contract, the Schematyc registration and the previewer path end to end;
//! stage 1 adds Sphere / Polygon / Spline beside it, the pairwise Incompatibility declarations and
//! IShapeComponentEdit.
//!
//! It also implements IShapeComponentEdit as a fixed two-point set - point 0 is the minimum
//! corner, point 1 the maximum corner, both in the component's local space - so that the generic
//! point tool of the editor plugin can resize a box without knowing it is a box. Neither
//! IShapeComponentEdit nor IEntityComponentPreviewer derives from IEntityComponent, so the
//! entity-component base stays unambiguous.
class CBoxShapeComponent final
	: public IShapeComponent
	, public IShapeComponentEdit
#ifndef RELEASE
	, public IEntityComponentPreviewer
#endif
{
public:
	CBoxShapeComponent() = default;
	virtual ~CBoxShapeComponent() = default;

	static void Register(Schematyc::CEnvRegistrationScope& componentScope);

	static void ReflectType(Schematyc::CTypeDesc<CBoxShapeComponent>& desc)
	{
		desc.SetGUID("{6C3A81D5-0F47-4E9A-B4D1-72E5C8A96201}"_cry_guid);
		desc.SetEditorCategory("Area");
		desc.SetLabel("Shape: Box");
		desc.SetDescription("An axis-aligned box shape. Area functions on the same entity use it as their volume.");
		desc.SetComponentFlags({ IEntityComponent::EFlags::Singleton });

		// The reflected interface bases: this is what makes GetComponent<IShapeComponent>() and
		// GetAllComponents<IEditorShapeComponent>() find us from another DLL (see the header
		// comment in IShapeComponent.h). IEditorShapeComponent is an indirect base - declaring it
		// here as well is what lets EditorQt query it without walking the base chain itself.
		desc.AddBase<IShapeComponent>();
		desc.AddBase<IEditorShapeComponent>();

		// One shape per entity (decision 01). One call, every other kind, both directions -
		// see ShapeKinds.h for why this is a list and not twelve hand-written declarations.
		DeclareShapeKindIncompatibilities(desc);

		desc.AddMember(&CBoxShapeComponent::m_size, 'size', "Size", "Size", "Full size of the box in metres", Vec3(1.0f, 1.0f, 1.0f));
		desc.AddMember(&CBoxShapeComponent::m_offset, 'offs', "Offset", "Offset", "Translation of the box centre in the entity's local space", Vec3(0.0f, 0.0f, 0.0f));
	}

	// IShapeComponent
	virtual EShapeKind GetKind() const override { return EShapeKind::Box; }

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

	// IShapeComponentEdit - a box has no point list, so it presents its two extreme corners as a
	// fixed two-point set: moving one of them resizes the box and keeps the other one still.
	virtual int  GetPointCount() const override { return 2; }
	virtual Vec3 GetPoint(int index) const override;
	virtual void SetPoint(int index, const Vec3& local) override;
	virtual int  InsertPoint(int index, const Vec3& local) override { return -1; } // fixed point set
	virtual void RemovePoint(int index) override                    {}             // fixed point set
	virtual void BeginEdit() override;
	virtual void EndEdit() override;
	//! The box's drawn footprint is its closed bottom quad.
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
	void NotifyListeners(EShapeChangeReason reason);

	//! World transform of the box centre: the component's world transform with the offset applied.
	Matrix34 GetShapeWorldTM() const;

	//! Smallest edge length a box may be resized to, so that a dragged corner can never collapse
	//! the box into a degenerate volume the editor can no longer pick.
	static constexpr float kMinSize = 0.01f;

	Vec3 m_size = Vec3(1.0f, 1.0f, 1.0f);
	Vec3 m_offset = ZERO;

	//! Set between BeginEdit() and EndEdit(): listeners are told once at the end of the gesture
	//! instead of once per mouse move.
	bool m_bInEdit = false;
	bool m_bChangedDuringEdit = false;

	//! Runtime only, never reflected: non-owning observers that un-register themselves.
	std::vector<IShapeListener*> m_listeners;
};

} // namespace AreaComponents
} // namespace Cry
