// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "CineGradeBinding.h"

#include <IEditor.h>
#include <IObjectManager.h>
#include <IUndoManager.h>
#include <Objects/BaseObject.h>
#include <Objects/ISelectionGroup.h>

#include <CryEntitySystem/IEntity.h>
#include <CryEntitySystem/IEntityComponent.h>
#include <CrySchematyc/IObject.h>
#include <CrySchematyc/IObjectProperties.h>
#include <CrySchematyc/Utils/ClassProperties.h>
#include <CrySchematyc/Reflection/TypeDesc.h>

namespace CineCamGrade
{

// ---------------------------------------------------------------------------
// The contract with the runtime plugin: GUIDs and member names, nothing else
// ---------------------------------------------------------------------------
// These are the only things this DLL knows about CinematicCamera.dll. Every one of them is a
// value that is already frozen for another reason - the component GUID is in every saved level,
// the member names are TrackView's persisted keys (item 3a) - so this is not a new promise, it is
// a use of an existing one. If any of them stops matching, Bind() refuses and says which.

//! CCineGradeComponent::ReflectType, Module/CineGradeComponent.h.
static const CryGUID kGradeComponentGuid = "{1E7C4A93-6B25-4D8F-9A03-52D7F1C86B40}"_cry_guid;
//! Module/CineCamLutTypes.h:119 - enum class ELutSpace : uint32.
static const CryGUID kLutSpaceGuid = "{2F6B8D50-91C3-4A7E-B248-05E7D9A16C3F}"_cry_guid;
//! Module/CineCamLutTypes.h:82 - SResourceNameSelector<...>, i.e. { string value; }.
static const CryGUID kLutFileGuid = "{9C21A4F7-3E58-4B06-8D19-6A5F0C7E24B3}"_cry_guid;
//! Module/CineGradeComponent.h - SCineGradePresetSlot, whose first member is the stored path.
static const CryGUID kPresetSlotGuid = "{5D8A1C64-0B37-4E92-A15F-3C6E80D74B29}"_cry_guid;
//! CrySchematyc/MathTypes.h:32 - ONE guid for every Range<...,float> instantiation, and the
//! struct is nothing but { float value; }, so a range member is a float at offset 0.
static const CryGUID kRangeFloatGuid = "{A3A15703-B420-42F0-97B7-4957B20CE376}"_cry_guid;

enum class EKind : uint8
{
	Bool,
	Float,      //!< float or Schematyc::Range<...,float>
	Vec3,
	LutSpace,   //!< ELutSpace, uint32
	LutPath,    //!< CineLutFileName  - a string at offset 0
	PresetPath, //!< SCineGradePresetSlot - the stored path is its first member
};

enum class EField : uint8
{
	Bypass,
	Lift, Gamma, Gain,
	LiftMaster, GammaMaster, GainMaster,
	Contrast, Pivot, Saturation,
	Slope, Offset, Power,
	MasterBlack, MasterShadow, MasterMid, MasterHighlight, MasterWhite,
	SatAtGrey, SatAtLow, SatAtMid, SatAtHigh, SatAtFull,
	LmtSpace, LmtFile, PresetPath,
};

struct SMemberEntry
{
	const char* szGroup;  //!< the component's own member name (Grade / Curves / Look / Preset)
	const char* szMember; //!< the member inside that struct - the frozen name from item 3a
	EKind       kind;
	EField      field;
};

//! Every value the panel shows. Order is display order and nothing depends on it.
static const SMemberEntry kMembers[] =
{
	{ "Grade",  "BypassGrade",     EKind::Bool,  EField::Bypass },
	{ "Grade",  "Lift",            EKind::Vec3,  EField::Lift },
	{ "Grade",  "LiftMaster",      EKind::Float, EField::LiftMaster },
	{ "Grade",  "Gamma",           EKind::Vec3,  EField::Gamma },
	{ "Grade",  "GammaMaster",     EKind::Float, EField::GammaMaster },
	{ "Grade",  "Gain",            EKind::Vec3,  EField::Gain },
	{ "Grade",  "GainMaster",      EKind::Float, EField::GainMaster },
	{ "Grade",  "Contrast",        EKind::Float, EField::Contrast },
	{ "Grade",  "ContrastPivot",   EKind::Float, EField::Pivot },
	{ "Grade",  "Saturation",      EKind::Float, EField::Saturation },
	{ "Grade",  "Slope",           EKind::Vec3,  EField::Slope },
	{ "Grade",  "Offset",          EKind::Vec3,  EField::Offset },
	{ "Grade",  "Power",           EKind::Vec3,  EField::Power },

	{ "Curves", "MasterBlack",     EKind::Float, EField::MasterBlack },
	{ "Curves", "MasterShadow",    EKind::Float, EField::MasterShadow },
	{ "Curves", "MasterMid",       EKind::Float, EField::MasterMid },
	{ "Curves", "MasterHighlight", EKind::Float, EField::MasterHighlight },
	{ "Curves", "MasterWhite",     EKind::Float, EField::MasterWhite },
	{ "Curves", "SatAtGrey",       EKind::Float, EField::SatAtGrey },
	{ "Curves", "SatAtLow",        EKind::Float, EField::SatAtLow },
	{ "Curves", "SatAtMid",        EKind::Float, EField::SatAtMid },
	{ "Curves", "SatAtHigh",       EKind::Float, EField::SatAtHigh },
	{ "Curves", "SatAtFull",       EKind::Float, EField::SatAtFull },

	{ "Look",   "LMTSpace",        EKind::LutSpace, EField::LmtSpace },
	{ "Look",   "LMTFile",         EKind::LutPath,  EField::LmtFile },

	{ "Preset", "",                EKind::PresetPath, EField::PresetPath },
};

//! The address of one field inside the panel's plain value struct. A switch and not offsetof():
//! SGradeValues holds strings, so it is not a standard-layout type.
static void* FieldAddr(SGradeValues& v, EField f)
{
	switch (f)
	{
	case EField::Bypass:          return &v.bypass;
	case EField::Lift:            return &v.lift;
	case EField::Gamma:           return &v.gamma;
	case EField::Gain:            return &v.gain;
	case EField::LiftMaster:      return &v.liftMaster;
	case EField::GammaMaster:     return &v.gammaMaster;
	case EField::GainMaster:      return &v.gainMaster;
	case EField::Contrast:        return &v.contrast;
	case EField::Pivot:           return &v.pivot;
	case EField::Saturation:      return &v.saturation;
	case EField::Slope:           return &v.slope;
	case EField::Offset:          return &v.offset;
	case EField::Power:           return &v.power;
	case EField::MasterBlack:     return &v.masterStops[0];
	case EField::MasterShadow:    return &v.masterStops[1];
	case EField::MasterMid:       return &v.masterStops[2];
	case EField::MasterHighlight: return &v.masterStops[3];
	case EField::MasterWhite:     return &v.masterStops[4];
	case EField::SatAtGrey:       return &v.satMul[0];
	case EField::SatAtLow:        return &v.satMul[1];
	case EField::SatAtMid:        return &v.satMul[2];
	case EField::SatAtHigh:       return &v.satMul[3];
	case EField::SatAtFull:       return &v.satMul[4];
	case EField::LmtSpace:        return &v.lmtSpace;
	case EField::LmtFile:         return &v.lmtFile;
	case EField::PresetPath:      return &v.presetPath;
	}
	return nullptr;
}

static bool TypeMatches(const Schematyc::CCommonTypeDesc& typeDesc, EKind kind)
{
	const CryGUID& guid = typeDesc.GetGUID();
	switch (kind)
	{
	case EKind::Bool:       return guid == Schematyc::GetTypeDesc<bool>().GetGUID();
	case EKind::Float:      return guid == Schematyc::GetTypeDesc<float>().GetGUID() || guid == kRangeFloatGuid;
	case EKind::Vec3:       return guid == Schematyc::GetTypeDesc<Vec3>().GetGUID();
	case EKind::LutSpace:   return guid == kLutSpaceGuid;
	case EKind::LutPath:    return guid == kLutFileGuid;
	case EKind::PresetPath: return guid == kPresetSlotGuid;
	}
	return false;
}

//! Walk the component's reflected members and hand the visitor (entry, pointer-into-component)
//! for every entry of kMembers that resolved with the expected type. Returns how many did.
//! A "Preset" entry with an empty member name means the group struct itself is the value, which
//! is how the preset slot's leading string is reached without knowing the rest of that struct.
template<typename TVisitor>
static int VisitMembers(IEntityComponent* pComponent, TVisitor&& visitor)
{
	int resolved = 0;
	const Schematyc::CClassDesc& classDesc = pComponent->GetClassDesc();
	for (const Schematyc::CClassMemberDesc& groupDesc : classDesc.GetMembers())
	{
		for (const SMemberEntry& entry : kMembers)
		{
			if (strcmp(groupDesc.GetName(), entry.szGroup) != 0)
				continue;

			if (entry.szMember[0] == '\0')
			{
				// The group itself is the value.
				if (!TypeMatches(groupDesc.GetTypeDesc(), entry.kind))
					continue;
				visitor(entry, groupDesc.GetOffsetPointer(pComponent));
				++resolved;
				continue;
			}

			if (groupDesc.GetTypeDesc().GetCategory() != Schematyc::ETypeCategory::Class)
				continue;

			const Schematyc::CClassDesc* pGroupClass = static_cast<const Schematyc::CClassDesc*>(&groupDesc.GetTypeDesc());
			void* pGroupBase = groupDesc.GetOffsetPointer(pComponent);
			for (const Schematyc::CClassMemberDesc& memberDesc : pGroupClass->GetMembers())
			{
				if (strcmp(memberDesc.GetName(), entry.szMember) != 0)
					continue;
				if (!TypeMatches(memberDesc.GetTypeDesc(), entry.kind))
					continue;
				visitor(entry, memberDesc.GetOffsetPointer(pGroupBase));
				++resolved;
				break;
			}
		}
	}
	return resolved;
}

static void ReadOne(const SMemberEntry& entry, const void* pSrc, SGradeValues& out)
{
	void* pDst = FieldAddr(out, entry.field);
	switch (entry.kind)
	{
	case EKind::Bool:     *static_cast<bool*>(pDst) = *static_cast<const bool*>(pSrc); break;
	case EKind::Float:    *static_cast<float*>(pDst) = *static_cast<const float*>(pSrc); break;
	case EKind::Vec3:     *static_cast<Vec3*>(pDst) = *static_cast<const Vec3*>(pSrc); break;
	case EKind::LutSpace: *static_cast<uint32*>(pDst) = *static_cast<const uint32*>(pSrc); break;
	case EKind::LutPath:
	case EKind::PresetPath:
		*static_cast<string*>(pDst) = *static_cast<const string*>(pSrc);
		break;
	}
}

//! Writes only what actually differs, so an unchanged string is never reallocated and an
//! unchanged float never makes the level look modified.
static void WriteOne(const SMemberEntry& entry, void* pDst, const SGradeValues& in)
{
	const void* pSrc = FieldAddr(const_cast<SGradeValues&>(in), entry.field);
	switch (entry.kind)
	{
	case EKind::Bool:
		if (*static_cast<bool*>(pDst) != *static_cast<const bool*>(pSrc))
			*static_cast<bool*>(pDst) = *static_cast<const bool*>(pSrc);
		break;
	case EKind::Float:
		if (*static_cast<float*>(pDst) != *static_cast<const float*>(pSrc))
			*static_cast<float*>(pDst) = *static_cast<const float*>(pSrc);
		break;
	case EKind::Vec3:
		if (*static_cast<Vec3*>(pDst) != *static_cast<const Vec3*>(pSrc))
			*static_cast<Vec3*>(pDst) = *static_cast<const Vec3*>(pSrc);
		break;
	case EKind::LutSpace:
		if (*static_cast<uint32*>(pDst) != *static_cast<const uint32*>(pSrc))
			*static_cast<uint32*>(pDst) = *static_cast<const uint32*>(pSrc);
		break;
	case EKind::LutPath:
		if (*static_cast<string*>(pDst) != *static_cast<const string*>(pSrc))
			*static_cast<string*>(pDst) = *static_cast<const string*>(pSrc);
		break;
	case EKind::PresetPath:
		// Read-only in the panel: the two preset buttons live on the component and cannot be
		// called from here (decisions section 5.2), and writing the path alone would show a
		// preset that was never applied.
		break;
	}
}

// ---------------------------------------------------------------------------
// SGradeValues
// ---------------------------------------------------------------------------

bool SGradeValues::operator==(const SGradeValues& rhs) const
{
	if (bypass != rhs.bypass || lift != rhs.lift || gamma != rhs.gamma || gain != rhs.gain)
		return false;
	if (liftMaster != rhs.liftMaster || gammaMaster != rhs.gammaMaster || gainMaster != rhs.gainMaster)
		return false;
	if (contrast != rhs.contrast || pivot != rhs.pivot || saturation != rhs.saturation)
		return false;
	if (slope != rhs.slope || offset != rhs.offset || power != rhs.power)
		return false;
	for (int i = 0; i < kKnotCount; ++i)
	{
		if (masterStops[i] != rhs.masterStops[i] || satMul[i] != rhs.satMul[i])
			return false;
	}
	return lmtSpace == rhs.lmtSpace && lmtFile == rhs.lmtFile && presetPath == rhs.presetPath;
}

// ---------------------------------------------------------------------------
// CEntityGradeTarget
// ---------------------------------------------------------------------------

CBaseObject* CEntityGradeTarget::ResolveObject() const
{
	if (m_objectGuid == CryGUID::Null())
		return nullptr;
	IObjectManager* pManager = GetIEditor()->GetObjectManager();
	return pManager ? pManager->FindObject(m_objectGuid) : nullptr;
}

IEntityComponent* CEntityGradeTarget::ResolveComponent() const
{
	CBaseObject* pObject = ResolveObject();
	IEntity* pEntity = pObject ? pObject->GetIEntity() : nullptr;
	return pEntity ? pEntity->GetComponentByTypeId(kGradeComponentGuid) : nullptr;
}

bool CEntityGradeTarget::Bind(CBaseObject* pObject, string& reason)
{
	m_objectGuid = CryGUID::Null();
	m_label.clear();
	m_undoOpen = false;

	IEntity* pEntity = pObject ? pObject->GetIEntity() : nullptr;
	if (pEntity == nullptr)
	{
		reason = "the selected object is not an entity";
		return false;
	}

	IEntityComponent* pComponent = pEntity->GetComponentByTypeId(kGradeComponentGuid);
	if (pComponent == nullptr)
	{
		reason = "the selected entity has no CineCam Grade component";
		return false;
	}

	// Resolve every member once, here, so that a mismatch is a refusal with a name in it rather
	// than a write to an offset we are not sure about.
	bool seen[CRY_ARRAY_COUNT(kMembers)] = { false };
	const int resolved = VisitMembers(pComponent, [&seen](const SMemberEntry& entry, void*)
	{
		for (size_t i = 0; i < CRY_ARRAY_COUNT(kMembers); ++i)
		{
			if (kMembers[i].field == entry.field)
				seen[i] = true;
		}
	});

	if (resolved != (int)CRY_ARRAY_COUNT(kMembers))
	{
		for (size_t i = 0; i < CRY_ARRAY_COUNT(kMembers); ++i)
		{
			if (!seen[i])
			{
				CryWarning(VALIDATOR_MODULE_EDITOR, VALIDATOR_WARNING,
					"[CineCam Grade panel] '%s/%s' is missing or has an unexpected type - the panel "
					"will not bind. The editor plugin and CinematicCamera.dll are out of step.",
					kMembers[i].szGroup, kMembers[i].szMember[0] ? kMembers[i].szMember : "(the group itself)");
			}
		}
		reason = "this build of the panel does not match CinematicCamera.dll (see the console)";
		return false;
	}

	m_objectGuid = pObject->GetId();
	m_label = pObject->GetName();
	return true;
}

bool CEntityGradeTarget::IsValid() const
{
	return ResolveComponent() != nullptr;
}

bool CEntityGradeTarget::Read(SGradeValues& out) const
{
	IEntityComponent* pComponent = ResolveComponent();
	if (pComponent == nullptr)
		return false;

	SGradeValues values;
	VisitMembers(pComponent, [&values](const SMemberEntry& entry, void* ptr)
	{
		ReadOne(entry, ptr, values);
	});
	out = values;
	return true;
}

void CEntityGradeTarget::BeginEdit()
{
	if (m_undoOpen)
		return;

	CBaseObject* pObject = ResolveObject();
	if (pObject == nullptr)
		return;

	// The same pair the entity inspector's property tree opens for every edit it makes
	// (ObjectPropertyWidget.cpp:151-159): the undo step is a snapshot of the whole object, which
	// includes the live components, so one Begin/StoreUndo ... Accept per drag is one Ctrl+Z.
	IUndoManager* pUndo = GetIEditor()->GetIUndoManager();
	if (pUndo == nullptr || pUndo->IsUndoRecording())
		return;

	pUndo->Begin();
	pObject->StoreUndo("CineCam Grade");
	m_undoOpen = true;
}

void CEntityGradeTarget::Write(const SGradeValues& in)
{
	IEntityComponent* pComponent = ResolveComponent();
	if (pComponent == nullptr)
		return;

	VisitMembers(pComponent, [&in](const SMemberEntry& entry, void* ptr)
	{
		WriteOne(entry, ptr, in);
	});

	// A write from outside a serialization pass is in the same position as a button: the editor's
	// save snapshot for a Schematyc entity is taken during the inspector's INPUT pass
	// (EntityObject.cpp:925-928), so it has to be refreshed by hand here. Harmless no-op for an
	// entity that has no Schematyc object - the level writes the live components in that case.
	if (IEntity* pEntity = pComponent->GetEntity())
	{
		if (Schematyc::IObject* pSchematycObject = pEntity->GetSchematycObject())
		{
			if (Schematyc::IObjectPropertiesPtr pProperties = pSchematycObject->GetObjectProperties())
			{
				if (Schematyc::CClassProperties* pClassProperties = pProperties->GetComponentProperties(pComponent->GetGUID()))
				{
					pClassProperties->SetOverridePolicy(Schematyc::EOverridePolicy::Override);
					pClassProperties->Read(pComponent->GetClassDesc(), pComponent);
				}
			}
		}
	}

	// Not the source of truth - the camera re-reads this component every frame - but it saves that
	// frame and, more usefully, it re-runs the component's ReportStatus(), which is the line that
	// says why a grade is not reaching the picture.
	SEntityEvent event(ENTITY_EVENT_COMPONENT_PROPERTY_CHANGED);
	pComponent->SendEvent(event);
}

void CEntityGradeTarget::EndEdit(bool bChanged)
{
	if (m_undoOpen)
	{
		IUndoManager* pUndo = GetIEditor()->GetIUndoManager();
		if (pUndo != nullptr)
		{
			if (bChanged)
				pUndo->Accept("CineCam Grade");
			else
				pUndo->Cancel();
		}
		m_undoOpen = false;
	}

	if (!bChanged)
		return;

	if (CBaseObject* pObject = ResolveObject())
	{
		pObject->SetModified(false, false);
		// Once per completed edit, never per mouse move: the inspector reloads on the next idle
		// through revertNoninterrupting(), which is a no-op while one of its own rows is captured.
		pObject->UpdateUIVars();
	}
}

// ---------------------------------------------------------------------------

CEntityGradeTarget* CreateTargetForSelection(string& reason)
{
	const ISelectionGroup* pSelection = GetIEditor()->GetISelectionGroup();
	const int count = pSelection ? pSelection->GetCount() : 0;

	if (count == 0)
	{
		reason = "nothing selected";
		return nullptr;
	}
	if (count > 1)
	{
		reason = "more than one object selected";
		return nullptr;
	}

	std::unique_ptr<CEntityGradeTarget> pTarget(new CEntityGradeTarget());
	if (!pTarget->Bind(pSelection->GetObject(0), reason))
		return nullptr;

	return pTarget.release();
}

} // namespace CineCamGrade
