// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#pragma once

// The controls the CineCam Grade panel is made of (S10 item 5). Nothing here knows about
// entities, components or undo - each control owns a number, says when a gesture starts, while it
// runs and when it ends, and the panel turns that into one undo transaction (CineGradeBinding.h).
//
// The three signals are the shape PropertyRowNumberField uses for a slider drag
// (Serialization/PropertyTreeLegacy/PropertyRowNumberField.cpp:49-115): open the transaction on
// the press, change the value as often as the mouse moves, close it on the release.
//
// Every draggable control here is RELATIVE (S10 item 5c). A press never moves the value; a drag
// adds mouse delta x step, so the number only ever changes by what the hand actually did, and a
// single pixel is a single step. This is the path already trodden inside CRYENGINE by
// PropertyRowNumberField::onMouseDrag (increment(pos.x() - lastMouseMove_.x())), by Resolve's
// colour wheels and its numeric scrub fields, and by a Lightroom slider. The one convention we do
// NOT copy from the legacy property tree is its modifiers - there Ctrl is finer and Shift coarser
// (PropertyRowNumber.h:226-234). Panel-wide, Shift is finer and Ctrl coarser, as in Resolve,
// Photoshop and Qt's own spin boxes.

#include <QDoubleSpinBox>
#include <QImage>
#include <QPoint>
#include <QPointF>
#include <QSlider>
#include <QWidget>
#include <CryMath/Cry_Math.h>

class QLabel;

namespace CineCamGrade
{

// ---------------------------------------------------------------------------------------------
// The step table - THE one place to retune how far the panel moves per unit of hand movement.
//
// One step = one pixel of drag = one mouse-wheel notch = one arrow key. Each number below is
// therefore also the finest edit that control can make by hand, and range / step is how many
// pixels a sweep of the whole range costs (500 - 1000 px here, i.e. deliberately a long sweep;
// N = 800 px for most of the panel). Typing a number is always exact and ignores all of this.
//
// Chosen from what a step does to the PICTURE, not from the width of the widget: Lift moves the
// shadows, where a thousandth of a code value is already a visible cast; Gain and Gamma are read
// as multipliers and can take five times more; the contrast pivot sits in a 0-1 ACEScct range so
// it gets a small step; the master curve knots are in stops, where a hundredth is about the
// smallest change an eye can find.
// ---------------------------------------------------------------------------------------------
namespace Step
{
//! Primaries - the R/G/B numbers beside each wheel, and the master beside them.
constexpr double kLiftChannel  = 0.001;
constexpr double kLiftMaster   = 0.002;
constexpr double kGammaChannel = 0.005;
constexpr double kGammaMaster  = 0.005;
constexpr double kGainChannel  = 0.005;
constexpr double kGainMaster   = 0.005;

//! Contrast and saturation.
constexpr double kContrast   = 0.005;
constexpr double kPivot      = 0.002;
constexpr double kSaturation = 0.005;

//! Curves - the master knots are in stops, the saturation knots are multipliers.
constexpr double kMasterStops = 0.01;
constexpr double kSatMul      = 0.005;

//! ASC CDL base.
constexpr double kCdlSlope  = 0.005;
constexpr double kCdlOffset = 0.001;
constexpr double kCdlPower  = 0.005;

//! The disc is the one control that is not stepped: it is a two-axis gesture, so it is tuned as a
//! fraction of itself. A full disc DIAMETER of mouse travel moves the puck this far, where 1.0 is
//! centre to rim - i.e. dragging the mouse right across the wheel spends only ~28 % of the
//! wheel's amplitude, and one pixel is a few ten-thousandths of a channel.
constexpr double kWheelDragGain = 0.28;
//! One wheel notch over a disc pushes the puck this much further out (or in) along its direction.
constexpr double kWheelDiscStep = 0.02;

//! Shift and Ctrl, the same factors on every control on the panel.
constexpr double kFineFactor   = 0.1;
constexpr double kCoarseFactor = 10.0;

//! How far the mouse must travel across a numeric field before it becomes a scrub instead of a
//! click that places the caret.
constexpr int kScrubThresholdPx = 3;
}

// ---------------------------------------------------------------------------------------------
// The sizes the panel and the wheels have to agree on. The disc follows the dock between the two
// bounds; the panel lowers the ceiling in its two-column layout (S10 item 5d), where the wheels
// share the width with every other parameter instead of owning all of it.
// ---------------------------------------------------------------------------------------------
namespace Layout
{
//! How small the disc may get before it stops being draggable, and how large it may get before it
//! is only costing repaint time. Between the two it follows the dock.
constexpr int kDiscMinSide = 72;
constexpr int kDiscMaxSide = 208;
//! Diameters are quantised to this, so the 16-odd pixels a scrollbar appearing takes away cannot
//! start a grow/shrink ping-pong between the disc and the scroll area.
constexpr int kDiscSideStep = 8;
//! The ceiling in the two-column layout: still a comfortable disc to drag, but the wheels stop
//! growing into the width the other parameters are there to use.
constexpr int kWideDiscMaxSide = 176;
}

//! A slider you SCRUB rather than place: the handle follows the mouse delta, never the mouse
//! position, so a press never teleports the value and one pixel is one step. Qt's own QSlider does
//! the opposite, which is why this exists. Wheel and arrow keys step; double-click and right-click
//! ask for neutral (the owner decides what neutral is).
class CCineScrubSlider : public QSlider
{
	Q_OBJECT

public:
	CCineScrubSlider(Qt::Orientation orientation, double step, QWidget* pParent = nullptr);

signals:
	void SignalBeginEdit();
	//! How much to add to the value, in the value's own units.
	void SignalScrubbed(double delta);
	void SignalEndEdit();
	void SignalReset();

protected:
	void mousePressEvent(QMouseEvent* pEvent) override;
	void mouseMoveEvent(QMouseEvent* pEvent) override;
	void mouseReleaseEvent(QMouseEvent* pEvent) override;
	void mouseDoubleClickEvent(QMouseEvent* pEvent) override;
	void wheelEvent(QWheelEvent* pEvent) override;
	void keyPressEvent(QKeyEvent* pEvent) override;

private:
	//! A discrete step is a whole gesture of its own, so it is one undo step just like a drag.
	void EmitStep(double delta);

	const double m_step;
	bool         m_scrubbing = false;
	QPoint       m_lastPos;
};

//! A number field you can scrub by dragging across it, the way grading and DCC number fields
//! behave. A click still places the caret and typing is still exact; only a drag past a few pixels
//! turns into a scrub. It emits the panel's three gesture signals instead of valueChanged, so a
//! whole scrub is one undo step and a typed number is one undo step.
class CCineScrubSpinBox : public QDoubleSpinBox
{
	Q_OBJECT

public:
	CCineScrubSpinBox(double minValue, double maxValue, int decimals, double step, QWidget* pParent = nullptr);

	//! Show a number without telling anyone - the panel is a view of the component, and showing
	//! what the component says must never look like an edit.
	void SetValueQuiet(double value);

signals:
	void SignalBeginEdit();
	void SignalChanged();
	void SignalEndEdit();

protected:
	bool eventFilter(QObject* pObject, QEvent* pEvent) override;
	void wheelEvent(QWheelEvent* pEvent) override;
	void keyPressEvent(QKeyEvent* pEvent) override;

private:
	void OnValueChanged();
	void StepValue(int notches, Qt::KeyboardModifiers modifiers);

	const double m_step;
	bool         m_pressed = false;
	bool         m_scrubbing = false;
	QPoint       m_pressPos;
	QPoint       m_lastPos;
	//! The scrub's own running value, kept at full precision so that a Shift-fine step smaller
	//! than the field's last decimal still accumulates instead of rounding away to nothing.
	double       m_scrubValue = 0.0;
};

//! A hue/saturation disc: direction = hue, distance from the centre = strength, centre = neutral.
//! It carries the mean-free part of an RGB triple, so it changes the COLOUR of a band and never
//! its level - that is what the master beside it is for.
class CCineWheelDisc : public QWidget
{
	Q_OBJECT

public:
	explicit CCineWheelDisc(QWidget* pParent = nullptr);

	//! Per-channel deviation at the rim of the disc. Lift wants a smaller one than Gain.
	void  SetAmplitude(float amplitude) { m_amplitude = amplitude; }
	//! The channel deviations from neutral. Only the mean-free part is shown; the numeric fields
	//! next to the disc remain the truth for anything else.
	void  SetDeviation(const Vec3& deviation);
	Vec3  GetDeviation() const;

	//! The width comes from the dock (the size policy is Expanding), the height is set to match by
	//! the control that owns the disc - that is what keeps it a circle at any dock width.
	QSize sizeHint() const override { return QSize(128, 128); }
	QSize minimumSizeHint() const override { return QSize(64, 64); }

signals:
	void SignalBeginEdit();
	void SignalChanged();
	void SignalEndEdit();

protected:
	void paintEvent(QPaintEvent* pEvent) override;
	void resizeEvent(QResizeEvent* pEvent) override;
	void mousePressEvent(QMouseEvent* pEvent) override;
	void mouseMoveEvent(QMouseEvent* pEvent) override;
	void mouseReleaseEvent(QMouseEvent* pEvent) override;
	void mouseDoubleClickEvent(QMouseEvent* pEvent) override;
	void wheelEvent(QWheelEvent* pEvent) override;

private:
	//! Add to the puck's position (normalised, y up) and keep it inside the disc.
	void   MovePuck(const QPointF& delta);
	void   Reset();
	QRectF DiscRect() const;

	//! Normalised puck position, y up, length clamped to 1.
	QPointF m_pos = QPointF(0.0, 0.0);
	float   m_amplitude = 0.5f;
	bool    m_dragging = false;
	QPoint  m_lastPos;
	QImage  m_wheelImage;
};

//! Disc + master + the four numbers, i.e. one of the three Primaries wheels.
class CCineWheelControl : public QWidget
{
	Q_OBJECT

public:
	//! neutral: 0 for Lift (the master is additive), 1 for Gamma and Gain (multiplicative).
	//! channelStep / masterStep: this band's rows of the step table above.
	CCineWheelControl(const QString& title, float neutral, float amplitude,
	                  float masterMin, float masterMax, float masterNeutral,
	                  double channelStep, double masterStep, QWidget* pParent = nullptr);

	void  SetValues(const Vec3& rgb, float master);
	Vec3  GetRgb() const    { return m_rgb; }
	float GetMaster() const { return m_master; }

	void  SetReadOnly(bool bReadOnly);

	//! Lower the disc's ceiling for a layout that has something else to do with the width.
	void  SetMaxDiscSide(int side);
	//! How wide this control must be for its disc to reach that diameter. The panel sizes its
	//! wheel column from this rather than from a hand-counted pixel total.
	int   WidthForDiscSide(int side) const;

signals:
	void SignalBeginEdit();
	void SignalChanged();
	void SignalEndEdit();

protected:
	void resizeEvent(QResizeEvent* pEvent) override;

private:
	//! Square the disc against the width this control was actually given.
	void UpdateDiscSize();
	//! Everything that stands beside the disc: the margins and the master slider.
	int  ReservedWidth() const;

	void OnDiscChanged();
	void OnChannelChanged();
	void OnMasterScrubbed(double delta);
	void OnMasterSpinChanged();
	void OnMasterReset();
	void ResetAll();
	void RefreshWidgets();
	void RefreshMaster();

	CCineWheelDisc*    m_pDisc = nullptr;
	CCineScrubSpinBox* m_pChannel[3] = { nullptr, nullptr, nullptr };
	CCineScrubSlider*  m_pMasterSlider = nullptr;
	CCineScrubSpinBox* m_pMasterSpin = nullptr;

	Vec3  m_rgb;
	float m_master = 0.0f;

	const float m_neutral;
	const float m_masterMin;
	const float m_masterMax;
	const float m_masterNeutral;

	int  m_maxDiscSide = Layout::kDiscMaxSide;
	bool m_updating = false;
};

//! Label + slider + numeric field, for the single floats (contrast, pivot, saturation, the ten
//! curve controls). Both the slider and the field scrub the same value by the same step.
class CCineFloatRow : public QWidget
{
	Q_OBJECT

public:
	CCineFloatRow(const QString& label, float minValue, float maxValue, float neutral,
	              int decimals, double step, QWidget* pParent = nullptr);

	void  SetValue(float value);
	float GetValue() const { return m_value; }
	void  SetReadOnly(bool bReadOnly);

signals:
	void SignalBeginEdit();
	void SignalChanged();
	void SignalEndEdit();

private:
	void OnScrubbed(double delta);
	void OnSpinChanged();
	void OnReset();
	void RefreshWidgets();

	CCineScrubSlider*  m_pSlider = nullptr;
	CCineScrubSpinBox* m_pSpin = nullptr;

	float m_value = 0.0f;
	const float m_min;
	const float m_max;
	const float m_neutral;

	bool m_updating = false;
};

} // namespace CineCamGrade
