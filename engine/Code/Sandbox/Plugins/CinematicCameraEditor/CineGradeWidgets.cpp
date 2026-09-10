// Copyright 2026 ReC Sandbox. Distributed under the terms in LICENSE.md at the repository root.
#include "StdAfx.h"
#include "CineGradeWidgets.h"

#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>

namespace CineCamGrade
{

// The three channel directions on the disc: red up, green and blue at 120 degrees clockwise from
// each other, which is the RGB/CMY wheel Resolve, Baselight and every vectorscope overlay use -
// yellow between red and green, cyan between green and blue, magenta between blue and red.
//
// deviation[c] = amplitude * ( y * cos(theta_c) + x * sin(theta_c) )
//
// The three cosines sum to zero for any direction, so the puck can never change the overall level
// of the band: at the centre the deviation is exactly zero on all three channels, which is what
// makes "the centre is neutral" a property of the mapping rather than a special case.
static const float kChannelAngle[3] = { 0.0f, 2.0f * gf_PI / 3.0f, 4.0f * gf_PI / 3.0f };

//! Shift makes any gesture ten times finer, Ctrl ten times coarser - the same everywhere on the
//! panel. It is read on every single mouse move rather than at the press, so pressing or releasing
//! a modifier in the middle of a drag changes the SPEED from that pixel on and can never make the
//! value jump: the drag only ever adds up what the hand has done since the last event.
static double ModifierGain(Qt::KeyboardModifiers modifiers)
{
	double gain = 1.0;
	if (modifiers & Qt::ShiftModifier)
		gain *= Step::kFineFactor;
	if (modifiers & Qt::ControlModifier)
		gain *= Step::kCoarseFactor;
	return gain;
}

//! One notch of a normal wheel is 120 units; a high-resolution wheel sends less, and a notch it
//! is still meant to be.
static int WheelNotches(const QWheelEvent* pEvent)
{
	const int delta = pEvent->angleDelta().y();
	if (delta == 0)
		return 0;
	const int notches = delta / 120;
	return (notches != 0) ? notches : (delta > 0 ? 1 : -1);
}

static Vec3 DeviationFromPos(const QPointF& pos, float amplitude)
{
	Vec3 deviation(ZERO);
	for (int c = 0; c < 3; ++c)
	{
		deviation[c] = amplitude * (float)(pos.y() * cos(kChannelAngle[c]) + pos.x() * sin(kChannelAngle[c]));
	}
	return deviation;
}

static QPointF PosFromDeviation(const Vec3& deviation, float amplitude)
{
	if (amplitude <= 0.0f)
		return QPointF(0.0, 0.0);

	// Drop the common part: only the colour of the offset can be shown on a disc.
	const float mean = (deviation.x + deviation.y + deviation.z) / 3.0f;
	double x = 0.0;
	double y = 0.0;
	for (int c = 0; c < 3; ++c)
	{
		const float d = deviation[c] - mean;
		x += d * sin(kChannelAngle[c]);
		y += d * cos(kChannelAngle[c]);
	}
	// The projection above scales by 3/2, so undo that to land back on the unit disc.
	const double scale = 2.0 / (3.0 * amplitude);
	QPointF pos(x * scale, y * scale);
	const double length = sqrt(pos.x() * pos.x() + pos.y() * pos.y());
	if (length > 1.0)
		pos /= length;
	return pos;
}

//! The same sentence under every draggable control, so the modifiers only have to be learnt once.
static QString ScrubHint(double step)
{
	return CCineScrubSlider::tr("Drag to scrub: one pixel = %1. Shift = ten times finer, Ctrl = ten times "
	                            "coarser, wheel or arrow keys = one step, double-click or right-click = neutral. "
	                            "Type a number for an exact value.").arg(step);
}

// ---------------------------------------------------------------------------
// CCineScrubSlider
// ---------------------------------------------------------------------------

CCineScrubSlider::CCineScrubSlider(Qt::Orientation orientation, double step, QWidget* pParent)
	: QSlider(orientation, pParent)
	, m_step(step)
{
	// The groove is only a picture of where the value sits inside its range; 1000 positions is
	// finer than the screen and the owner keeps the real float.
	setRange(0, 1000);
	setCursor(orientation == Qt::Horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
	setToolTip(ScrubHint(step));
}

void CCineScrubSlider::EmitStep(double delta)
{
	SignalBeginEdit();
	SignalScrubbed(delta);
	SignalEndEdit();
}

void CCineScrubSlider::mousePressEvent(QMouseEvent* pEvent)
{
	if (!isEnabled())
	{
		pEvent->ignore();
		return;
	}

	if (pEvent->button() == Qt::RightButton)
	{
		SignalReset();
		pEvent->accept();
		return;
	}
	if (pEvent->button() != Qt::LeftButton)
	{
		pEvent->ignore();
		return;
	}

	// Deliberately NOT QSlider::mousePressEvent: Qt would either page towards the click or put the
	// handle under it, and both of those are a jump. The press only opens the gesture; from here
	// the value moves with the hand and with nothing else.
	m_scrubbing = true;
	m_lastPos = pEvent->pos();
	SignalBeginEdit();
	pEvent->accept();
}

void CCineScrubSlider::mouseMoveEvent(QMouseEvent* pEvent)
{
	if (!m_scrubbing)
	{
		pEvent->ignore();
		return;
	}

	const QPoint pos = pEvent->pos();
	// Up is more on a vertical slider, right is more on a horizontal one.
	const int pixels = (orientation() == Qt::Horizontal) ? (pos.x() - m_lastPos.x()) : (m_lastPos.y() - pos.y());
	m_lastPos = pos;
	if (pixels != 0)
		SignalScrubbed(pixels * m_step * ModifierGain(pEvent->modifiers()));
	pEvent->accept();
}

void CCineScrubSlider::mouseReleaseEvent(QMouseEvent* pEvent)
{
	if (m_scrubbing && pEvent->button() == Qt::LeftButton)
	{
		m_scrubbing = false;
		SignalEndEdit();
		pEvent->accept();
		return;
	}
	pEvent->ignore();
}

void CCineScrubSlider::mouseDoubleClickEvent(QMouseEvent* pEvent)
{
	if (isEnabled() && pEvent->button() == Qt::LeftButton)
	{
		m_scrubbing = false;
		SignalReset();
		pEvent->accept();
		return;
	}
	pEvent->ignore();
}

void CCineScrubSlider::wheelEvent(QWheelEvent* pEvent)
{
	const int notches = isEnabled() ? WheelNotches(pEvent) : 0;
	if (notches == 0)
	{
		// Let the scroll area have it rather than swallowing a scroll that did nothing here.
		pEvent->ignore();
		return;
	}

	EmitStep(notches * m_step * ModifierGain(pEvent->modifiers()));
	pEvent->accept();
}

void CCineScrubSlider::keyPressEvent(QKeyEvent* pEvent)
{
	if (isEnabled())
	{
		const int key = pEvent->key();
		if (key == Qt::Key_Left || key == Qt::Key_Down || key == Qt::Key_Right || key == Qt::Key_Up)
		{
			const double direction = (key == Qt::Key_Right || key == Qt::Key_Up) ? 1.0 : -1.0;
			EmitStep(direction * m_step * ModifierGain(pEvent->modifiers()));
			pEvent->accept();
			return;
		}
	}
	QSlider::keyPressEvent(pEvent);
}

// ---------------------------------------------------------------------------
// CCineScrubSpinBox
// ---------------------------------------------------------------------------

CCineScrubSpinBox::CCineScrubSpinBox(double minValue, double maxValue, int decimals, double step, QWidget* pParent)
	: QDoubleSpinBox(pParent)
	, m_step(step)
{
	setRange(minValue, maxValue);
	setDecimals(decimals);
	setSingleStep(step);
	// The panel wants whole numbers, not one write per keystroke: the value is committed on Enter
	// or when the field loses focus.
	setKeyboardTracking(false);
	setMinimumWidth(62);
	setToolTip(ScrubHint(step));

	// The text area is a QLineEdit child, so the drag has to be caught there - a mousePressEvent
	// on the spin box itself never sees a press on its own text.
	if (QLineEdit* pEdit = lineEdit())
		pEdit->installEventFilter(this);

	connect(this, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
	        this, &CCineScrubSpinBox::OnValueChanged);
}

void CCineScrubSpinBox::SetValueQuiet(double value)
{
	const QSignalBlocker blocker(this);
	setValue(value);
}

void CCineScrubSpinBox::OnValueChanged()
{
	if (m_scrubbing)
	{
		// The gesture opened the transaction on its first moved pixel and closes it on the release.
		SignalChanged();
		return;
	}

	// A typed number, a wheel notch, an arrow key or the up/down buttons: a gesture of its own.
	SignalBeginEdit();
	SignalChanged();
	SignalEndEdit();
}

void CCineScrubSpinBox::StepValue(int notches, Qt::KeyboardModifiers modifiers)
{
	// Half-typed text is committed first, exactly as QAbstractSpinBox::stepBy does it.
	interpretText();

	const double delta = notches * m_step * ModifierGain(modifiers);
	setValue(clamp_tpl(value() + delta, minimum(), maximum()));
}

bool CCineScrubSpinBox::eventFilter(QObject* pObject, QEvent* pEvent)
{
	if (pObject == lineEdit() && isEnabled() && !isReadOnly())
	{
		switch (pEvent->type())
		{
		case QEvent::MouseButtonPress:
			{
				QMouseEvent* pMouse = static_cast<QMouseEvent*>(pEvent);
				if (pMouse->button() == Qt::LeftButton)
				{
					m_pressed = true;
					m_scrubbing = false;
					m_pressPos = pMouse->pos();
					m_lastPos = m_pressPos;
				}
				// Not swallowed: a press that turns out to be a click must still place the caret.
				break;
			}
		case QEvent::MouseMove:
			{
				if (!m_pressed)
					break;

				QMouseEvent* pMouse = static_cast<QMouseEvent*>(pEvent);
				const QPoint pos = pMouse->pos();
				if (!m_scrubbing)
				{
					if (qAbs(pos.x() - m_pressPos.x()) < Step::kScrubThresholdPx)
						break;

					m_scrubbing = true;
					m_scrubValue = value();
					lineEdit()->deselect();
					SignalBeginEdit();
				}

				const int pixels = pos.x() - m_lastPos.x();
				m_lastPos = pos;
				if (pixels != 0)
				{
					m_scrubValue = clamp_tpl(m_scrubValue + pixels * m_step * ModifierGain(pMouse->modifiers()),
					                         minimum(), maximum());
					setValue(m_scrubValue);
				}
				return true;
			}
		case QEvent::MouseButtonRelease:
			{
				const bool bWasScrubbing = m_scrubbing;
				m_pressed = false;
				m_scrubbing = false;
				if (bWasScrubbing)
				{
					SignalEndEdit();
					// Swallowed, so the release cannot also move the caret or start a selection.
					return true;
				}
				break;
			}
		default:
			break;
		}
	}
	return QDoubleSpinBox::eventFilter(pObject, pEvent);
}

void CCineScrubSpinBox::wheelEvent(QWheelEvent* pEvent)
{
	const int notches = (isEnabled() && !isReadOnly()) ? WheelNotches(pEvent) : 0;
	if (notches == 0)
	{
		pEvent->ignore();
		return;
	}

	// Not QDoubleSpinBox::wheelEvent: that one steps by singleStep and knows only Ctrl (coarser),
	// so Shift would do nothing here while it makes every other control on the panel finer.
	StepValue(notches, pEvent->modifiers());
	pEvent->accept();
}

void CCineScrubSpinBox::keyPressEvent(QKeyEvent* pEvent)
{
	const int key = pEvent->key();
	if ((key == Qt::Key_Up || key == Qt::Key_Down) && isEnabled() && !isReadOnly())
	{
		StepValue(key == Qt::Key_Up ? 1 : -1, pEvent->modifiers());
		pEvent->accept();
		return;
	}
	QDoubleSpinBox::keyPressEvent(pEvent);
}

// ---------------------------------------------------------------------------
// CCineWheelDisc
// ---------------------------------------------------------------------------

CCineWheelDisc::CCineWheelDisc(QWidget* pParent)
	: QWidget(pParent)
{
	// Horizontally the disc takes whatever the dock gives it; the height is driven to match by
	// CCineWheelControl::UpdateDiscSize, so the disc is square - and equally large on all three
	// wheels - at every dock width without ever pushing its own minimum around.
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setCursor(Qt::CrossCursor);
	setToolTip(tr("Drag for hue and strength - the puck follows the hand, so crossing the whole wheel "
	              "spends only about a quarter of its strength. Shift = ten times finer, Ctrl = ten times "
	              "coarser, wheel = further out or in. Double-click or right-click to go back to neutral."));
}

void CCineWheelDisc::SetDeviation(const Vec3& deviation)
{
	m_pos = PosFromDeviation(deviation, m_amplitude);
	update();
}

Vec3 CCineWheelDisc::GetDeviation() const
{
	return DeviationFromPos(m_pos, m_amplitude);
}

QRectF CCineWheelDisc::DiscRect() const
{
	const qreal side = qMin(width(), height()) - 4.0;
	return QRectF((width() - side) * 0.5, (height() - side) * 0.5, side, side);
}

void CCineWheelDisc::resizeEvent(QResizeEvent* pEvent)
{
	QWidget::resizeEvent(pEvent);

	// The disc is a picture of the mapping above, so it is built from the same formula rather than
	// from a hand-tuned gradient: a pixel shows the colour its own offset would push towards.
	const QRectF rect = DiscRect();
	const int side = qMax(8, (int)rect.width());
	// A resize that did not change the disc's diameter (only its box) keeps the picture it has:
	// the loop below is O(side^2) and a dock drag delivers a resize every frame.
	if (!m_wheelImage.isNull() && m_wheelImage.width() == side)
		return;

	QImage image(side, side, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);

	const double radius = side * 0.5;
	for (int py = 0; py < side; ++py)
	{
		for (int px = 0; px < side; ++px)
		{
			const double nx = (px + 0.5 - radius) / radius;
			const double ny = (radius - py - 0.5) / radius;
			const double length = sqrt(nx * nx + ny * ny);
			if (length > 1.0)
				continue;

			const Vec3 hue = DeviationFromPos(QPointF(nx, ny), 1.0f);
			const int r = (int)clamp_tpl(128.0f + hue.x * 127.0f, 0.0f, 255.0f);
			const int g = (int)clamp_tpl(128.0f + hue.y * 127.0f, 0.0f, 255.0f);
			const int b = (int)clamp_tpl(128.0f + hue.z * 127.0f, 0.0f, 255.0f);
			// One pixel of feathering at the rim, so the disc has no staircase edge.
			const double alpha = clamp_tpl((1.0 - length) * radius, 0.0, 1.0);
			const int a = (int)(alpha * 255.0);
			image.setPixel(px, py, qRgba((r * a) / 255, (g * a) / 255, (b * a) / 255, a));
		}
	}
	m_wheelImage = image;
}

void CCineWheelDisc::paintEvent(QPaintEvent* /*pEvent*/)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing, true);

	const QRectF rect = DiscRect();
	if (!m_wheelImage.isNull())
		painter.drawImage(rect, m_wheelImage);

	painter.setBrush(Qt::NoBrush);
	painter.setPen(QPen(QColor(24, 24, 24), 1.0));
	painter.drawEllipse(rect);

	const QPointF centre = rect.center();
	const qreal radius = rect.width() * 0.5;

	// A cross at the centre, so "neutral" is visible even when the puck sits on it.
	painter.setPen(QPen(QColor(0, 0, 0, 110), 1.0));
	painter.drawLine(QPointF(centre.x() - 4.0, centre.y()), QPointF(centre.x() + 4.0, centre.y()));
	painter.drawLine(QPointF(centre.x(), centre.y() - 4.0), QPointF(centre.x(), centre.y() + 4.0));

	const QPointF puck(centre.x() + m_pos.x() * radius, centre.y() - m_pos.y() * radius);

	painter.setPen(QPen(QColor(255, 255, 255, 160), 1.0));
	painter.drawLine(centre, puck);

	painter.setPen(QPen(QColor(20, 20, 20), 1.5));
	painter.setBrush(QColor(245, 245, 245));
	painter.drawEllipse(puck, 4.5, 4.5);

	if (!isEnabled())
	{
		painter.setPen(Qt::NoPen);
		painter.setBrush(QColor(40, 40, 40, 140));
		painter.drawEllipse(rect);
	}
}

void CCineWheelDisc::MovePuck(const QPointF& delta)
{
	QPointF pos = m_pos + delta;
	const double length = sqrt(pos.x() * pos.x() + pos.y() * pos.y());
	if (length > 1.0)
		pos /= length;

	if (pos == m_pos)
		return;

	m_pos = pos;
	update();
	SignalChanged();
}

void CCineWheelDisc::Reset()
{
	SignalBeginEdit();
	m_pos = QPointF(0.0, 0.0);
	update();
	SignalChanged();
	SignalEndEdit();
}

void CCineWheelDisc::mousePressEvent(QMouseEvent* pEvent)
{
	if (!isEnabled())
		return;

	if (pEvent->button() == Qt::RightButton)
	{
		Reset();
		return;
	}
	if (pEvent->button() != Qt::LeftButton)
		return;

	// The puck stays where it is: a wheel is a trackball, not a map. Pressing anywhere on the disc
	// picks the puck up, and from there it moves by what the hand does - which is what makes a
	// small correction to an existing offset possible at all, and what Resolve, Baselight and
	// every hardware panel do.
	m_dragging = true;
	m_lastPos = pEvent->pos();
	SignalBeginEdit();
}

void CCineWheelDisc::mouseMoveEvent(QMouseEvent* pEvent)
{
	if (!m_dragging)
		return;

	const QPoint pos = pEvent->pos();
	const QPoint pixels = pos - m_lastPos;
	m_lastPos = pos;
	if (pixels.isNull())
		return;

	// Per diameter, not per pixel: the gain is the same gesture on a 72 px disc in a narrow dock
	// and on a 208 px one in a wide dock, and a bigger disc simply gets finer pixels.
	const double diameter = qMax(1.0, (double)DiscRect().width());
	const double gain = Step::kWheelDragGain * ModifierGain(pEvent->modifiers()) / diameter;
	MovePuck(QPointF(pixels.x() * gain, -pixels.y() * gain));
}

void CCineWheelDisc::mouseReleaseEvent(QMouseEvent* pEvent)
{
	if (m_dragging && pEvent->button() == Qt::LeftButton)
	{
		m_dragging = false;
		SignalEndEdit();
	}
}

void CCineWheelDisc::mouseDoubleClickEvent(QMouseEvent* pEvent)
{
	if (isEnabled() && pEvent->button() == Qt::LeftButton)
	{
		m_dragging = false;
		Reset();
	}
}

void CCineWheelDisc::wheelEvent(QWheelEvent* pEvent)
{
	const double length = sqrt(m_pos.x() * m_pos.x() + m_pos.y() * m_pos.y());
	const int notches = isEnabled() ? WheelNotches(pEvent) : 0;
	// A neutral wheel has no direction to push along, so the notch belongs to the scroll area -
	// which also means scrolling the panel past three untouched wheels still works.
	if (notches == 0 || length < 1e-4)
	{
		pEvent->ignore();
		return;
	}

	const double target = clamp_tpl(length + notches * Step::kWheelDiscStep * ModifierGain(pEvent->modifiers()),
	                                0.0, 1.0);
	SignalBeginEdit();
	m_pos *= (target / length);
	update();
	SignalChanged();
	SignalEndEdit();
	pEvent->accept();
}

// ---------------------------------------------------------------------------
// CCineWheelControl
// ---------------------------------------------------------------------------

// The disc's bounds live in CineGradeWidgets.h, namespace Layout: the panel picks the ceiling for
// the layout it is in, so both sides have to name the same numbers.

CCineWheelControl::CCineWheelControl(const QString& title, float neutral, float amplitude,
                                     float masterMin, float masterMax, float masterNeutral,
                                     double channelStep, double masterStep, QWidget* pParent)
	: QWidget(pParent)
	, m_rgb(neutral, neutral, neutral)
	, m_master(masterNeutral)
	, m_neutral(neutral)
	, m_masterMin(masterMin)
	, m_masterMax(masterMax)
	, m_masterNeutral(masterNeutral)
{
	m_pDisc = new CCineWheelDisc(this);
	m_pDisc->SetAmplitude(amplitude);

	m_pMasterSlider = new CCineScrubSlider(Qt::Vertical, masterStep, this);
	m_pMasterSlider->setToolTip(tr("Master: the whole band at once.") + QStringLiteral("\n") + m_pMasterSlider->toolTip());

	QLabel* pTitle = new QLabel(title, this);
	pTitle->setAlignment(Qt::AlignCenter);
	QFont titleFont = pTitle->font();
	titleFont.setBold(true);
	pTitle->setFont(titleFont);

	QPushButton* pReset = new QPushButton(tr("Reset"), this);
	pReset->setToolTip(tr("Back to neutral - the same as double-clicking the wheel."));

	static const char* const szChannel[3] = { "R", "G", "B" };
	QGridLayout* pNumbers = new QGridLayout();
	pNumbers->setContentsMargins(0, 0, 0, 0);
	pNumbers->setHorizontalSpacing(4);
	pNumbers->setVerticalSpacing(2);
	// The four fields follow the wheel's width instead of leaving a gap beside it.
	pNumbers->setColumnStretch(1, 1);
	for (int c = 0; c < 3; ++c)
	{
		// Wide enough for a real grade in either direction; the component itself does not clamp
		// the triples, and TrackView keys are not clamped either.
		m_pChannel[c] = new CCineScrubSpinBox(neutral - 2.0, neutral + 2.0, 4, channelStep, this);
		pNumbers->addWidget(new QLabel(QString::fromLatin1(szChannel[c]), this), c, 0);
		pNumbers->addWidget(m_pChannel[c], c, 1);
	}
	m_pMasterSpin = new CCineScrubSpinBox(masterMin, masterMax, 4, masterStep, this);
	pNumbers->addWidget(new QLabel(tr("M"), this), 3, 0);
	pNumbers->addWidget(m_pMasterSpin, 3, 1);

	QHBoxLayout* pDiscRow = new QHBoxLayout();
	pDiscRow->setContentsMargins(0, 0, 0, 0);
	pDiscRow->addWidget(m_pDisc, 1);
	pDiscRow->addWidget(m_pMasterSlider);

	QVBoxLayout* pLayout = new QVBoxLayout(this);
	pLayout->setContentsMargins(4, 4, 4, 4);
	pLayout->setSpacing(4);
	pLayout->addWidget(pTitle);
	pLayout->addLayout(pDiscRow);
	pLayout->addLayout(pNumbers);
	pLayout->addWidget(pReset);
	setLayout(pLayout);

	connect(m_pDisc, &CCineWheelDisc::SignalBeginEdit, this, &CCineWheelControl::SignalBeginEdit);
	connect(m_pDisc, &CCineWheelDisc::SignalEndEdit, this, &CCineWheelControl::SignalEndEdit);
	connect(m_pDisc, &CCineWheelDisc::SignalChanged, this, &CCineWheelControl::OnDiscChanged);

	for (int c = 0; c < 3; ++c)
	{
		connect(m_pChannel[c], &CCineScrubSpinBox::SignalBeginEdit, this, &CCineWheelControl::SignalBeginEdit);
		connect(m_pChannel[c], &CCineScrubSpinBox::SignalChanged, this, &CCineWheelControl::OnChannelChanged);
		connect(m_pChannel[c], &CCineScrubSpinBox::SignalEndEdit, this, &CCineWheelControl::SignalEndEdit);
	}

	connect(m_pMasterSpin, &CCineScrubSpinBox::SignalBeginEdit, this, &CCineWheelControl::SignalBeginEdit);
	connect(m_pMasterSpin, &CCineScrubSpinBox::SignalChanged, this, &CCineWheelControl::OnMasterSpinChanged);
	connect(m_pMasterSpin, &CCineScrubSpinBox::SignalEndEdit, this, &CCineWheelControl::SignalEndEdit);

	connect(m_pMasterSlider, &CCineScrubSlider::SignalBeginEdit, this, &CCineWheelControl::SignalBeginEdit);
	connect(m_pMasterSlider, &CCineScrubSlider::SignalScrubbed, this, &CCineWheelControl::OnMasterScrubbed);
	connect(m_pMasterSlider, &CCineScrubSlider::SignalEndEdit, this, &CCineWheelControl::SignalEndEdit);
	connect(m_pMasterSlider, &CCineScrubSlider::SignalReset, this, &CCineWheelControl::OnMasterReset);

	connect(pReset, &QPushButton::clicked, this, &CCineWheelControl::ResetAll);

	RefreshWidgets();
}

void CCineWheelControl::SetValues(const Vec3& rgb, float master)
{
	m_rgb = rgb;
	m_master = master;
	RefreshWidgets();
}

void CCineWheelControl::SetReadOnly(bool bReadOnly)
{
	setEnabled(!bReadOnly);
}

void CCineWheelControl::resizeEvent(QResizeEvent* pEvent)
{
	QWidget::resizeEvent(pEvent);
	UpdateDiscSize();
}

void CCineWheelControl::UpdateDiscSize()
{
	// The disc is sized from the width the layout actually handed this control, minus the margins,
	// the master slider beside it and the gap between the two - never from a size hint. The three
	// controls share the panel's width equally, so the three discs stay the same size.
	//
	// Only the disc's MAXIMUM width is moved (its minimum stays at minimumSizeHint): if the
	// minimum grew with the disc, this control could no longer shrink and the panel would be stuck
	// at its widest ever size.
	int side = clamp_tpl(width() - ReservedWidth(), Layout::kDiscMinSide, m_maxDiscSide);
	side -= side % Layout::kDiscSideStep;

	if (m_pDisc->maximumWidth() == side && m_pDisc->height() == side)
		return;

	m_pDisc->setMaximumWidth(side);
	m_pDisc->setFixedHeight(side);
}

int CCineWheelControl::ReservedWidth() const
{
	return 8 + m_pMasterSlider->sizeHint().width() + 6;
}

int CCineWheelControl::WidthForDiscSide(int side) const
{
	return side + ReservedWidth();
}

void CCineWheelControl::SetMaxDiscSide(int side)
{
	side = clamp_tpl(side, Layout::kDiscMinSide, Layout::kDiscMaxSide);
	if (side == m_maxDiscSide)
		return;

	m_maxDiscSide = side;
	UpdateDiscSize();
}

void CCineWheelControl::RefreshMaster()
{
	m_pMasterSpin->SetValueQuiet(m_master);
	const float t = (m_masterMax > m_masterMin) ? (m_master - m_masterMin) / (m_masterMax - m_masterMin) : 0.0f;
	m_pMasterSlider->setValue((int)(clamp_tpl(t, 0.0f, 1.0f) * 1000.0f + 0.5f));
}

void CCineWheelControl::RefreshWidgets()
{
	m_updating = true;
	m_pDisc->SetDeviation(m_rgb - Vec3(m_neutral, m_neutral, m_neutral));
	for (int c = 0; c < 3; ++c)
		m_pChannel[c]->SetValueQuiet(m_rgb[c]);
	RefreshMaster();
	m_updating = false;
}

void CCineWheelControl::OnDiscChanged()
{
	if (m_updating)
		return;

	const Vec3 deviation = m_pDisc->GetDeviation();
	m_rgb = Vec3(m_neutral, m_neutral, m_neutral) + deviation;

	for (int c = 0; c < 3; ++c)
		m_pChannel[c]->SetValueQuiet(m_rgb[c]);

	SignalChanged();
}

void CCineWheelControl::OnChannelChanged()
{
	if (m_updating)
		return;

	for (int c = 0; c < 3; ++c)
		m_rgb[c] = (float)m_pChannel[c]->value();

	m_pDisc->SetDeviation(m_rgb - Vec3(m_neutral, m_neutral, m_neutral));

	SignalChanged();
}

void CCineWheelControl::OnMasterScrubbed(double delta)
{
	if (m_updating)
		return;

	m_master = (float)clamp_tpl((double)m_master + delta, (double)m_masterMin, (double)m_masterMax);
	RefreshMaster();

	SignalChanged();
}

void CCineWheelControl::OnMasterSpinChanged()
{
	if (m_updating)
		return;

	m_master = (float)m_pMasterSpin->value();
	const float t = (m_masterMax > m_masterMin) ? (m_master - m_masterMin) / (m_masterMax - m_masterMin) : 0.0f;
	m_pMasterSlider->setValue((int)(clamp_tpl(t, 0.0f, 1.0f) * 1000.0f + 0.5f));

	SignalChanged();
}

void CCineWheelControl::OnMasterReset()
{
	if (!isEnabled())
		return;

	SignalBeginEdit();
	m_master = m_masterNeutral;
	RefreshMaster();
	SignalChanged();
	SignalEndEdit();
}

void CCineWheelControl::ResetAll()
{
	if (!isEnabled())
		return;

	SignalBeginEdit();
	m_rgb = Vec3(m_neutral, m_neutral, m_neutral);
	m_master = m_masterNeutral;
	RefreshWidgets();
	SignalChanged();
	SignalEndEdit();
}

// ---------------------------------------------------------------------------
// CCineFloatRow
// ---------------------------------------------------------------------------

CCineFloatRow::CCineFloatRow(const QString& label, float minValue, float maxValue, float neutral,
                             int decimals, double step, QWidget* pParent)
	: QWidget(pParent)
	, m_value(neutral)
	, m_min(minValue)
	, m_max(maxValue)
	, m_neutral(neutral)
{
	QLabel* pLabel = new QLabel(label, this);
	pLabel->setMinimumWidth(120);

	m_pSlider = new CCineScrubSlider(Qt::Horizontal, step, this);
	m_pSpin = new CCineScrubSpinBox(minValue, maxValue, decimals, step, this);

	QHBoxLayout* pLayout = new QHBoxLayout(this);
	pLayout->setContentsMargins(0, 0, 0, 0);
	pLayout->setSpacing(6);
	pLayout->addWidget(pLabel);
	pLayout->addWidget(m_pSlider, 1);
	pLayout->addWidget(m_pSpin);
	setLayout(pLayout);

	connect(m_pSlider, &CCineScrubSlider::SignalBeginEdit, this, &CCineFloatRow::SignalBeginEdit);
	connect(m_pSlider, &CCineScrubSlider::SignalScrubbed, this, &CCineFloatRow::OnScrubbed);
	connect(m_pSlider, &CCineScrubSlider::SignalEndEdit, this, &CCineFloatRow::SignalEndEdit);
	connect(m_pSlider, &CCineScrubSlider::SignalReset, this, &CCineFloatRow::OnReset);

	connect(m_pSpin, &CCineScrubSpinBox::SignalBeginEdit, this, &CCineFloatRow::SignalBeginEdit);
	connect(m_pSpin, &CCineScrubSpinBox::SignalChanged, this, &CCineFloatRow::OnSpinChanged);
	connect(m_pSpin, &CCineScrubSpinBox::SignalEndEdit, this, &CCineFloatRow::SignalEndEdit);

	RefreshWidgets();
}

void CCineFloatRow::SetValue(float value)
{
	m_value = value;
	RefreshWidgets();
}

void CCineFloatRow::SetReadOnly(bool bReadOnly)
{
	setEnabled(!bReadOnly);
}

void CCineFloatRow::RefreshWidgets()
{
	m_updating = true;
	m_pSpin->SetValueQuiet(m_value);
	const float t = (m_max > m_min) ? (m_value - m_min) / (m_max - m_min) : 0.0f;
	m_pSlider->setValue((int)(clamp_tpl(t, 0.0f, 1.0f) * 1000.0f + 0.5f));
	m_updating = false;
}

void CCineFloatRow::OnScrubbed(double delta)
{
	if (m_updating)
		return;

	// The row's float is the truth, not the slider's 1000 positions: a Shift-fine scrub can move
	// the value by less than one groove position and still be a real edit.
	m_value = (float)clamp_tpl((double)m_value + delta, (double)m_min, (double)m_max);
	RefreshWidgets();

	SignalChanged();
}

void CCineFloatRow::OnSpinChanged()
{
	if (m_updating)
		return;

	m_value = (float)m_pSpin->value();

	m_updating = true;
	const float t = (m_max > m_min) ? (m_value - m_min) / (m_max - m_min) : 0.0f;
	m_pSlider->setValue((int)(clamp_tpl(t, 0.0f, 1.0f) * 1000.0f + 0.5f));
	m_updating = false;

	SignalChanged();
}

void CCineFloatRow::OnReset()
{
	if (!isEnabled())
		return;

	SignalBeginEdit();
	m_value = m_neutral;
	RefreshWidgets();
	SignalChanged();
	SignalEndEdit();
}

} // namespace CineCamGrade
