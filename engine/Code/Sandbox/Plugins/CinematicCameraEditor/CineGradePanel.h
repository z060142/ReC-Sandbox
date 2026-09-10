// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

// The CineCam Grade panel (S10 item 5): the three Primaries wheels and every other grade control
// of the selected entity's CineCam Grade, in one window - Tools -> Cinematic Camera ->
// CineCam Grade.
//
// It is a VIEW of the component and never a second copy of it: every editor idle it re-reads the
// values and repaints if they moved, so an edit in the entity inspector, an undo, a preset apply
// and (later) a TrackView scrub all show up here without this panel knowing they happened. It
// only owns the values while the user is actually dragging something in it.
//
// Binding, and the rule the user set: exactly one selected entity that has a CineCam Grade ->
// editable, the header names it. Anything else -> read-only and greyed, header says why, the last
// numbers stay on screen.

#include <EditorFramework/Editor.h>
#include <IEditor.h>

#include "CineGradeBinding.h"

#include <memory>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QScrollArea;
class QVBoxLayout;

namespace CineCamGrade
{

//! What the width of the dock leaves room for. The controls are the same objects in all three -
//! only where they are parented changes - so a reflow never rebuilds or writes anything.
enum class EGradeLayout
{
	//! One column, the three wheels stacked.
	Narrow,
	//! One column, the three wheels in a row.
	Medium,
	//! Two columns: the wheels on the left, every other parameter on the right.
	Wide,
};

class CCineWheelControl;
class CCineFloatRow;
class CCineScrubSpinBox;

class CCineGradePanel : public CDockableEditor, public IEditorNotifyListener
{
	Q_OBJECT

public:
	explicit CCineGradePanel(QWidget* pParent = nullptr);
	virtual ~CCineGradePanel();

	// CDockableEditor
	virtual const char* GetEditorName() const override { return "CineCam Grade"; }
	// ~CDockableEditor

	// IEditorNotifyListener
	virtual void OnEditorNotifyEvent(EEditorNotifyEvent event) override;
	// ~IEditorNotifyListener

protected:
	//! Two breakpoints, three layouts - see EGradeLayout.
	virtual void resizeEvent(QResizeEvent* pEvent) override;

private:
	QWidget* BuildWheels();
	QWidget* BuildTone();
	QWidget* BuildCurves();
	QWidget* BuildCdlBase();
	QWidget* BuildLook();

	//! Put the three wheels in one row or in one column. Layout only - the controls are the same
	//! objects either way, so a reflow in the middle of a drag would still be harmless.
	void ApplyWheelLayout(bool bStacked);

	//! Move the non-wheel controls between the single column and the right-hand one, and give the
	//! wheels the disc ceiling of the layout they land in. Same widgets throughout: nothing is
	//! rebuilt, no value is collected and nothing is written to the component.
	void ApplyLayout(EGradeLayout layout);
	//! Two-column layout only: how wide the wheel column may be at the current dock width.
	void UpdateWheelColumnWidth();
	//! The width at which three wheels reach Layout::kWideDiscMaxSide, margins included.
	int  WheelColumnCap() const;

	//! Hook one control's three signals into the one transaction the panel keeps.
	template<typename TControl> void Wire(TControl* pControl);

	void Rebind(bool bForce);
	void Poll();
	void SetBoundState(const char* szReason);

	//! One undo step per gesture: opened on the first change of a drag, closed on the release.
	void BeginInteraction();
	void PushValues();
	void EndInteraction();

	//! Widgets -> m_values (called at the start of every push) and m_values -> widgets.
	void CollectFromWidgets();
	void RefreshWidgets();

	void OnBrowseLmt();

	std::unique_ptr<CEntityGradeTarget> m_pTarget;
	SGradeValues                        m_values;

	CCineWheelControl* m_pLift = nullptr;
	CCineWheelControl* m_pGamma = nullptr;
	CCineWheelControl* m_pGain = nullptr;

	CCineFloatRow*  m_pContrast = nullptr;
	CCineFloatRow*  m_pPivot = nullptr;
	CCineFloatRow*  m_pSaturation = nullptr;
	CCineFloatRow*  m_pMasterCurve[kKnotCount] = { nullptr };
	CCineFloatRow*  m_pSatCurve[kKnotCount] = { nullptr };
	//! Slope / Offset / Power, three channels each.
	CCineScrubSpinBox* m_pCdl[3][3] = { { nullptr } };

	QCheckBox* m_pBypass = nullptr;
	QLabel*    m_pHeader = nullptr;
	QLineEdit* m_pLmtFile = nullptr;
	QComboBox* m_pLmtSpace = nullptr;
	QLineEdit* m_pPreset = nullptr;
	QWidget*   m_pControls = nullptr;

	//! Every non-wheel control in one widget, so that switching layouts is one reparent.
	QWidget*     m_pRestColumn = nullptr;
	QVBoxLayout* m_pLeftHostLayout = nullptr;
	QVBoxLayout* m_pRightHostLayout = nullptr;
	QScrollArea* m_pLeftScroll = nullptr;
	QScrollArea* m_pRightScroll = nullptr;

	QGridLayout* m_pWheelLayout = nullptr;
	bool         m_wheelsStacked = false;
	EGradeLayout m_layout = EGradeLayout::Medium;

	bool m_interacting = false;
	bool m_changedInInteraction = false;
	bool m_updatingWidgets = false;
	int  m_rebindCountdown = 0;
};

} // namespace CineCamGrade
