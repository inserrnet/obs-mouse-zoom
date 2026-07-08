/*
OBS Mouse Zoom
Copyright (C) 2026

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <plugin-support.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("inserrnet")

namespace {

constexpr const char *kSettingsFile = "settings.json";
constexpr const char *kModeSelectedFirst = "selected_first";
constexpr const char *kModeTopVisible = "top_visible";

struct Settings {
	bool enabled = true;
	int speed = 5;
	double smoothness = 0.25;
	double minZoom = 0.25;
	double maxZoom = 8.0;
	std::string mode = kModeSelectedFirst;
};

struct CanvasPoint {
	double x = 0.0;
	double y = 0.0;
};

struct AlignOffset {
	double x = 0.0;
	double y = 0.0;
};

struct ZoomAnimation {
	obs_sceneitem_t *item = nullptr;
	CanvasPoint anchorCanvas;
	CanvasPoint anchorLocal;
	AlignOffset alignOffset;
	struct vec2 targetScale = {};
	uint32_t width = 0;
	uint32_t height = 0;
};

class MouseZoomPlugin : public QObject {
public:
	MouseZoomPlugin()
	{
		loadSettings();

		timer.setInterval(16);
		timer.setTimerType(Qt::PreciseTimer);
		connect(&timer, &QTimer::timeout, this, [this]() { tickAnimation(); });

		if (qApp) {
			qApp->installEventFilter(this);
		}

		obs_frontend_add_tools_menu_item("OBS Mouse Zoom", showSettings, this);
		obs_log(LOG_INFO, "OBS Mouse Zoom loaded");
	}

	~MouseZoomPlugin() override
	{
		if (qApp) {
			qApp->removeEventFilter(this);
		}

		stopAnimation();
		saveSettings();
		obs_log(LOG_INFO, "OBS Mouse Zoom unloaded");
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (event->type() != QEvent::Wheel || !settings.enabled) {
			return QObject::eventFilter(watched, event);
		}

		auto *wheel = static_cast<QWheelEvent *>(event);
		QWidget *display = previewDisplayForObject(watched);
		if (!display) {
			return QObject::eventFilter(watched, event);
		}

		const QPoint displayPoint = display->mapFromGlobal(wheel->globalPosition().toPoint());
		CanvasPoint canvas;
		if (!displayToCanvas(display, displayPoint, canvas)) {
			return QObject::eventFilter(watched, event);
		}

		const int delta = wheel->angleDelta().y();
		obs_log(LOG_INFO, "wheel over preview: delta=%d canvas=(%.1f, %.1f)", delta, canvas.x, canvas.y);

		if (delta == 0) {
			return true;
		}

		zoomAt(canvas, delta);
		wheel->accept();
		return true;
	}

	void openSettingsDialog()
	{
		if (settingsDialog) {
			settingsDialog->raise();
			settingsDialog->activateWindow();
			return;
		}

		QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
		auto *dialog = new QDialog(parent);
		settingsDialog = dialog;
		dialog->setWindowTitle("OBS Mouse Zoom");
		dialog->setAttribute(Qt::WA_DeleteOnClose);

		auto *layout = new QVBoxLayout(dialog);
		auto *form = new QFormLayout();

		auto *enableBox = new QCheckBox(dialog);
		enableBox->setChecked(settings.enabled);
		form->addRow("Enable", enableBox);

		auto *speedSlider = new QSlider(Qt::Horizontal, dialog);
		speedSlider->setRange(1, 10);
		speedSlider->setValue(settings.speed);
		auto *speedSpin = new QSpinBox(dialog);
		speedSpin->setRange(1, 10);
		speedSpin->setValue(settings.speed);
		auto *speedRow = new QWidget(dialog);
		auto *speedLayout = new QHBoxLayout(speedRow);
		speedLayout->setContentsMargins(0, 0, 0, 0);
		speedLayout->addWidget(speedSlider);
		speedLayout->addWidget(speedSpin);
		form->addRow("Speed", speedRow);

		auto *smoothSpin = new QDoubleSpinBox(dialog);
		smoothSpin->setRange(0.05, 1.0);
		smoothSpin->setSingleStep(0.05);
		smoothSpin->setDecimals(2);
		smoothSpin->setValue(settings.smoothness);
		form->addRow("Smoothness", smoothSpin);

		auto *minSpin = new QDoubleSpinBox(dialog);
		minSpin->setRange(0.01, 100.0);
		minSpin->setSingleStep(0.05);
		minSpin->setDecimals(2);
		minSpin->setValue(settings.minZoom);
		form->addRow("Min Zoom", minSpin);

		auto *maxSpin = new QDoubleSpinBox(dialog);
		maxSpin->setRange(0.01, 100.0);
		maxSpin->setSingleStep(0.25);
		maxSpin->setDecimals(2);
		maxSpin->setValue(settings.maxZoom);
		form->addRow("Max Zoom", maxSpin);

		auto *modeCombo = new QComboBox(dialog);
		modeCombo->addItem("Selected source first", kModeSelectedFirst);
		modeCombo->addItem("Top visible source", kModeTopVisible);
		modeCombo->setCurrentIndex(settings.mode == kModeTopVisible ? 1 : 0);
		form->addRow("Mode", modeCombo);

		auto *resetButton = new QPushButton("Reset selected/top source zoom", dialog);
		form->addRow("", resetButton);

		layout->addLayout(form);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
		layout->addWidget(buttons);

		connect(enableBox, &QCheckBox::toggled, dialog, [this](bool checked) {
			settings.enabled = checked;
			saveSettings();
		});
		connect(speedSlider, &QSlider::valueChanged, speedSpin, &QSpinBox::setValue);
		connect(speedSpin, qOverload<int>(&QSpinBox::valueChanged), speedSlider, &QSlider::setValue);
		connect(speedSpin, qOverload<int>(&QSpinBox::valueChanged), dialog, [this](int value) {
			settings.speed = value;
			saveSettings();
		});
		connect(smoothSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), dialog, [this](double value) {
			settings.smoothness = value;
			saveSettings();
		});
		connect(minSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), dialog,
			[this, maxSpin](double value) {
				settings.minZoom = value;
				if (settings.maxZoom < settings.minZoom) {
					settings.maxZoom = settings.minZoom;
					maxSpin->setValue(settings.maxZoom);
				}
				saveSettings();
			});
		connect(maxSpin, qOverload<double>(&QDoubleSpinBox::valueChanged), dialog,
			[this, minSpin](double value) {
				settings.maxZoom = value;
				if (settings.minZoom > settings.maxZoom) {
					settings.minZoom = settings.maxZoom;
					minSpin->setValue(settings.minZoom);
				}
				saveSettings();
			});
		connect(modeCombo, qOverload<int>(&QComboBox::currentIndexChanged), dialog, [this, modeCombo](int) {
			settings.mode = modeCombo->currentData().toString().toStdString();
			saveSettings();
		});
		connect(resetButton, &QPushButton::clicked, dialog, [this]() { resetCurrentItemZoom(); });
		connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
		connect(dialog, &QObject::destroyed, this, [this]() { settingsDialog = nullptr; });

		dialog->show();
	}

private:
	static void showSettings(void *data) { static_cast<MouseZoomPlugin *>(data)->openSettingsDialog(); }

	static bool isPreviewClass(QObject *object)
	{
		for (QObject *current = object; current; current = current->parent()) {
			const QString className = current->metaObject()->className();
			if (className.contains("OBSQTDisplay") || className.contains("OBSBasicPreview")) {
				return true;
			}
		}

		return false;
	}

	static QWidget *previewDisplayForObject(QObject *object)
	{
		auto *widget = qobject_cast<QWidget *>(object);
		if (!widget || !isPreviewClass(widget)) {
			return nullptr;
		}

		QWidget *mainWindow = static_cast<QWidget *>(obs_frontend_get_main_window());
		if (mainWindow && widget != mainWindow && !mainWindow->isAncestorOf(widget)) {
			return nullptr;
		}

		for (QWidget *current = widget; current; current = current->parentWidget()) {
			const QString className = current->metaObject()->className();
			if (className.contains("OBSQTDisplay")) {
				return current;
			}
		}

		return widget;
	}

	static bool displayToCanvas(QWidget *display, const QPoint &displayPoint, CanvasPoint &canvas)
	{
		struct obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0) {
			obs_log(LOG_WARNING, "unable to read OBS base canvas size");
			return false;
		}

		const double widgetWidth = display->width();
		const double widgetHeight = display->height();
		const double scale =
			std::min(widgetWidth / double(ovi.base_width), widgetHeight / double(ovi.base_height));
		if (scale <= 0.0) {
			return false;
		}

		const double previewWidth = double(ovi.base_width) * scale;
		const double previewHeight = double(ovi.base_height) * scale;
		const double offsetX = (widgetWidth - previewWidth) / 2.0;
		const double offsetY = (widgetHeight - previewHeight) / 2.0;
		const double x = (double(displayPoint.x()) - offsetX) / scale;
		const double y = (double(displayPoint.y()) - offsetY) / scale;

		if (x < 0.0 || y < 0.0 || x > double(ovi.base_width) || y > double(ovi.base_height)) {
			return false;
		}

		canvas = {x, y};
		return true;
	}

	static AlignOffset alignmentOffset(uint32_t alignment, uint32_t width, uint32_t height)
	{
		AlignOffset offset;

		if (alignment & OBS_ALIGN_RIGHT) {
			offset.x = double(width);
		} else if (!(alignment & OBS_ALIGN_LEFT)) {
			offset.x = double(width) / 2.0;
		}

		if (alignment & OBS_ALIGN_BOTTOM) {
			offset.y = double(height);
		} else if (!(alignment & OBS_ALIGN_TOP)) {
			offset.y = double(height) / 2.0;
		}

		return offset;
	}

	static bool selectedItemCallback(obs_scene_t *, obs_sceneitem_t *item, void *data)
	{
		auto **selected = static_cast<obs_sceneitem_t **>(data);
		if (obs_sceneitem_visible(item) && obs_sceneitem_selected(item)) {
			if (*selected) {
				obs_sceneitem_release(*selected);
			}
			obs_sceneitem_addref(item);
			*selected = item;
		}

		return true;
	}

	static bool topVisibleItemCallback(obs_scene_t *, obs_sceneitem_t *item, void *data)
	{
		auto **topVisible = static_cast<obs_sceneitem_t **>(data);
		if (obs_sceneitem_visible(item)) {
			if (*topVisible) {
				obs_sceneitem_release(*topVisible);
			}
			obs_sceneitem_addref(item);
			*topVisible = item;
		}

		return true;
	}

	obs_sceneitem_t *currentTargetItem() const
	{
		obs_source_t *sceneSource = obs_frontend_get_current_preview_scene();
		if (!sceneSource) {
			sceneSource = obs_frontend_get_current_scene();
		}
		if (!sceneSource) {
			obs_log(LOG_WARNING, "no current OBS scene");
			return nullptr;
		}

		obs_scene_t *scene = obs_scene_from_source(sceneSource);
		obs_sceneitem_t *item = nullptr;

		if (scene) {
			if (settings.mode != kModeTopVisible) {
				obs_scene_enum_items(scene, selectedItemCallback, &item);
			}
			if (!item) {
				obs_scene_enum_items(scene, topVisibleItemCallback, &item);
			}
		}

		obs_source_release(sceneSource);

		if (!item) {
			obs_log(LOG_WARNING, "no visible scene item to zoom");
		}

		return item;
	}

	static bool itemGeometry(obs_sceneitem_t *item, struct obs_transform_info &info, uint32_t &width,
				 uint32_t &height)
	{
		if (!item) {
			return false;
		}

		obs_source_t *source = obs_sceneitem_get_source(item);
		if (!source) {
			return false;
		}

		width = obs_source_get_width(source);
		height = obs_source_get_height(source);
		if (width == 0 || height == 0) {
			obs_log(LOG_WARNING, "source has zero size, skipping zoom");
			return false;
		}

		obs_sceneitem_get_info2(item, &info);
		if (std::fabs(info.scale.x) < 0.0001 || std::fabs(info.scale.y) < 0.0001) {
			obs_log(LOG_WARNING, "scene item has near-zero scale, skipping zoom");
			return false;
		}

		return true;
	}

	static CanvasPoint canvasToLocal(const CanvasPoint &canvas, const struct obs_transform_info &info,
					 const AlignOffset &offset)
	{
		return {
			offset.x + ((canvas.x - double(info.pos.x)) / double(info.scale.x)),
			offset.y + ((canvas.y - double(info.pos.y)) / double(info.scale.y)),
		};
	}

	static struct vec2 anchoredPosition(const CanvasPoint &canvas, const CanvasPoint &local,
					    const AlignOffset &offset, const struct vec2 &scale)
	{
		struct vec2 pos = {};
		pos.x = float(canvas.x - ((local.x - offset.x) * double(scale.x)));
		pos.y = float(canvas.y - ((local.y - offset.y) * double(scale.y)));
		return pos;
	}

	void zoomAt(const CanvasPoint &canvas, int wheelDelta)
	{
		obs_sceneitem_t *item = currentTargetItem();
		if (!item) {
			return;
		}

		struct obs_transform_info info = {};
		uint32_t width = 0;
		uint32_t height = 0;
		if (!itemGeometry(item, info, width, height)) {
			obs_sceneitem_release(item);
			return;
		}

		const AlignOffset offset = alignmentOffset(info.alignment, width, height);
		const CanvasPoint local = canvasToLocal(canvas, info, offset);

		const double direction = double(wheelDelta) / 120.0;
		const double speedFactor = std::max(1, settings.speed) / 5.0;
		const double factor = std::pow(1.10, direction * speedFactor);
		const double currentAbsX = std::fabs(info.scale.x);
		const double previousTargetAbsX = (animation.item == item) ? std::fabs(animation.targetScale.x)
									   : currentAbsX;
		const double targetAbsX = std::clamp(previousTargetAbsX * factor, settings.minZoom, settings.maxZoom);
		const double ratio = targetAbsX / currentAbsX;

		if (animation.item != item) {
			stopAnimation();
			obs_sceneitem_addref(item);
			animation.item = item;
		}

		animation.anchorCanvas = canvas;
		animation.anchorLocal = local;
		animation.alignOffset = offset;
		animation.width = width;
		animation.height = height;
		animation.targetScale.x = float(double(info.scale.x) * ratio);
		animation.targetScale.y = float(double(info.scale.y) * ratio);

		if (!timer.isActive()) {
			timer.start();
		}

		obs_sceneitem_release(item);
	}

	void tickAnimation()
	{
		if (!animation.item) {
			timer.stop();
			return;
		}

		struct obs_transform_info info = {};
		uint32_t width = 0;
		uint32_t height = 0;
		if (!itemGeometry(animation.item, info, width, height)) {
			stopAnimation();
			return;
		}

		const double alpha = std::clamp(settings.smoothness, 0.05, 1.0);
		struct vec2 nextScale = {};
		nextScale.x =
			float(double(info.scale.x) + (double(animation.targetScale.x) - double(info.scale.x)) * alpha);
		nextScale.y =
			float(double(info.scale.y) + (double(animation.targetScale.y) - double(info.scale.y)) * alpha);

		const double remainingX = std::fabs(double(animation.targetScale.x) - double(nextScale.x));
		const double remainingY = std::fabs(double(animation.targetScale.y) - double(nextScale.y));
		if (remainingX < 0.0005 && remainingY < 0.0005) {
			nextScale = animation.targetScale;
		}

		info.scale = nextScale;
		info.pos = anchoredPosition(animation.anchorCanvas, animation.anchorLocal, animation.alignOffset,
					    nextScale);
		obs_sceneitem_set_info2(animation.item, &info);

		if (nextScale.x == animation.targetScale.x && nextScale.y == animation.targetScale.y) {
			stopAnimation();
		}
	}

	void resetCurrentItemZoom()
	{
		obs_sceneitem_t *item = currentTargetItem();
		if (!item) {
			return;
		}

		stopAnimation();

		struct obs_transform_info info = {};
		uint32_t width = 0;
		uint32_t height = 0;
		if (!itemGeometry(item, info, width, height)) {
			obs_sceneitem_release(item);
			return;
		}

		const AlignOffset offset = alignmentOffset(info.alignment, width, height);
		const CanvasPoint localCenter = {double(width) / 2.0, double(height) / 2.0};
		const CanvasPoint canvasCenter = {
			double(info.pos.x) + ((localCenter.x - offset.x) * double(info.scale.x)),
			double(info.pos.y) + ((localCenter.y - offset.y) * double(info.scale.y)),
		};

		const double signX = info.scale.x < 0.0f ? -1.0 : 1.0;
		const double signY = info.scale.y < 0.0f ? -1.0 : 1.0;
		info.scale.x = float(signX);
		info.scale.y = float(signY);
		info.pos = anchoredPosition(canvasCenter, localCenter, offset, info.scale);
		obs_sceneitem_set_info2(item, &info);
		obs_sceneitem_release(item);
	}

	void stopAnimation()
	{
		timer.stop();
		if (animation.item) {
			obs_sceneitem_release(animation.item);
			animation = {};
		}
	}

	std::string settingsPath() const
	{
		char *path = obs_module_config_path(kSettingsFile);
		if (!path) {
			return {};
		}

		std::string result = path;
		bfree(path);
		return result;
	}

	void loadSettings()
	{
		const std::string path = settingsPath();
		if (path.empty()) {
			obs_log(LOG_WARNING, "settings path unavailable");
			return;
		}

		obs_data_t *data = obs_data_create_from_json_file(path.c_str());
		if (!data) {
			return;
		}

		obs_data_set_default_bool(data, "enabled", settings.enabled);
		obs_data_set_default_int(data, "speed", settings.speed);
		obs_data_set_default_double(data, "smoothness", settings.smoothness);
		obs_data_set_default_double(data, "min_zoom", settings.minZoom);
		obs_data_set_default_double(data, "max_zoom", settings.maxZoom);
		obs_data_set_default_string(data, "mode", settings.mode.c_str());

		settings.enabled = obs_data_get_bool(data, "enabled");
		settings.speed = int(obs_data_get_int(data, "speed"));
		settings.smoothness = obs_data_get_double(data, "smoothness");
		settings.minZoom = obs_data_get_double(data, "min_zoom");
		settings.maxZoom = obs_data_get_double(data, "max_zoom");
		settings.mode = obs_data_get_string(data, "mode");

		settings.speed = std::clamp(settings.speed, 1, 10);
		settings.smoothness = std::clamp(settings.smoothness, 0.05, 1.0);
		settings.minZoom = std::max(0.01, settings.minZoom);
		settings.maxZoom = std::max(settings.minZoom, settings.maxZoom);
		if (settings.mode != kModeTopVisible) {
			settings.mode = kModeSelectedFirst;
		}

		obs_data_release(data);
	}

	void saveSettings() const
	{
		const std::string path = settingsPath();
		if (path.empty()) {
			return;
		}

		char *dir = obs_module_config_path("");
		if (dir) {
			os_mkdirs(dir);
			bfree(dir);
		}

		obs_data_t *data = obs_data_create();
		obs_data_set_bool(data, "enabled", settings.enabled);
		obs_data_set_int(data, "speed", settings.speed);
		obs_data_set_double(data, "smoothness", settings.smoothness);
		obs_data_set_double(data, "min_zoom", settings.minZoom);
		obs_data_set_double(data, "max_zoom", settings.maxZoom);
		obs_data_set_string(data, "mode", settings.mode.c_str());

		if (!obs_data_save_json(data, path.c_str())) {
			obs_log(LOG_WARNING, "failed to save settings to %s", path.c_str());
		}

		obs_data_release(data);
	}

	Settings settings;
	QTimer timer;
	ZoomAnimation animation;
	QPointer<QDialog> settingsDialog;
};

std::unique_ptr<MouseZoomPlugin> plugin;

} // namespace

bool obs_module_load(void)
{
	plugin = std::make_unique<MouseZoomPlugin>();
	return true;
}

void obs_module_unload(void)
{
	plugin.reset();
}
