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
#include <graphics/matrix4.h>
#include <util/platform.h>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
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
constexpr double kPreviewEdgeSize = 10.0;

struct Settings {
	bool enabled = true;
	bool debugLogging = false;
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

struct PreviewMapping {
	double displayX = 0.0;
	double displayY = 0.0;
	double widgetWidth = 0.0;
	double widgetHeight = 0.0;
	double pixelRatio = 1.0;
	double previewX = 0.0;
	double previewY = 0.0;
	double previewScale = 1.0;
	uint32_t baseWidth = 0;
	uint32_t baseHeight = 0;
};

struct ZoomAnimation {
	obs_sceneitem_t *item = nullptr;
	CanvasPoint anchorCanvas;
	CanvasPoint anchorLocal;
	struct vec2 targetScale = {};
	uint32_t width = 0;
	uint32_t height = 0;
	int settleFrames = 0;
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

		const QPointF displayPoint = display->mapFromGlobal(wheel->globalPosition());
		CanvasPoint canvas;
		PreviewMapping mapping;
		if (!displayToCanvas(display, displayPoint, canvas, mapping)) {
			return QObject::eventFilter(watched, event);
		}

		const int delta = wheel->angleDelta().y();
		obs_log(LOG_DEBUG, "wheel over preview: delta=%d canvas=(%.1f, %.1f)", delta, canvas.x, canvas.y);

		if (delta == 0) {
			return true;
		}

		if (settings.debugLogging) {
			obs_log(LOG_INFO,
				"preview map: display=(%.2f, %.2f) widget=(%.1f, %.1f) dpr=%.3f preview=(%.2f, %.2f scale=%.6f) base=(%u, %u) canvas=(%.2f, %.2f)",
				mapping.displayX, mapping.displayY, mapping.widgetWidth, mapping.widgetHeight,
				mapping.pixelRatio, mapping.previewX, mapping.previewY, mapping.previewScale,
				mapping.baseWidth, mapping.baseHeight, canvas.x, canvas.y);
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

		auto *debugBox = new QCheckBox(dialog);
		debugBox->setChecked(settings.debugLogging);
		form->addRow("Debug logging", debugBox);

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
		connect(debugBox, &QCheckBox::toggled, dialog, [this](bool checked) {
			settings.debugLogging = checked;
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

	static bool displayToCanvas(QWidget *display, const QPointF &displayPoint, CanvasPoint &canvas,
				    PreviewMapping &mapping)
	{
		struct obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi) || ovi.base_width == 0 || ovi.base_height == 0) {
			obs_log(LOG_WARNING, "unable to read OBS base canvas size");
			return false;
		}

		const double pixelRatio = display->devicePixelRatioF();
		const double widgetWidth = std::round(double(display->width()) * pixelRatio);
		const double widgetHeight = std::round(double(display->height()) * pixelRatio);
		const double availableWidth = widgetWidth - (kPreviewEdgeSize * 2.0);
		const double availableHeight = widgetHeight - (kPreviewEdgeSize * 2.0);
		if (availableWidth <= 0.0 || availableHeight <= 0.0 || pixelRatio <= 0.0) {
			return false;
		}

		const double previewScale =
			std::min(availableWidth / double(ovi.base_width), availableHeight / double(ovi.base_height));
		if (previewScale <= 0.0) {
			return false;
		}

		const double previewWidth = double(ovi.base_width) * previewScale;
		const double previewHeight = double(ovi.base_height) * previewScale;
		const double previewX = ((availableWidth - previewWidth) / 2.0) + kPreviewEdgeSize;
		const double previewY = ((availableHeight - previewHeight) / 2.0) + kPreviewEdgeSize;
		const double x = ((displayPoint.x() * pixelRatio) - previewX) / previewScale;
		const double y = ((displayPoint.y() * pixelRatio) - previewY) / previewScale;

		mapping.displayX = displayPoint.x();
		mapping.displayY = displayPoint.y();
		mapping.widgetWidth = widgetWidth;
		mapping.widgetHeight = widgetHeight;
		mapping.pixelRatio = pixelRatio;
		mapping.previewX = previewX;
		mapping.previewY = previewY;
		mapping.previewScale = previewScale;
		mapping.baseWidth = ovi.base_width;
		mapping.baseHeight = ovi.base_height;

		if (x < 0.0 || y < 0.0 || x > double(ovi.base_width) || y > double(ovi.base_height)) {
			return false;
		}

		canvas = {x, y};
		return true;
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

	static bool sourceLocalFromCanvas(obs_sceneitem_t *item, const CanvasPoint &canvas, CanvasPoint &local)
	{
		struct matrix4 transform = {};
		struct matrix4 inverse = {};
		struct vec3 canvasPoint = {};
		struct vec3 localPoint = {};

		obs_sceneitem_get_draw_transform(item, &transform);
		if (std::fabs(matrix4_determinant(&transform)) < 0.000001f) {
			return false;
		}

		matrix4_inv(&inverse, &transform);
		canvasPoint.x = float(canvas.x);
		canvasPoint.y = float(canvas.y);
		canvasPoint.z = 0.0f;
		vec3_transform(&localPoint, &canvasPoint, &inverse);

		if (!std::isfinite(localPoint.x) || !std::isfinite(localPoint.y)) {
			return false;
		}

		local.x = double(localPoint.x);
		local.y = double(localPoint.y);
		return true;
	}

	static CanvasPoint canvasFromSourceLocal(obs_sceneitem_t *item, const CanvasPoint &local)
	{
		struct matrix4 transform = {};
		struct vec3 localPoint = {};
		struct vec3 canvasPoint = {};

		obs_sceneitem_get_draw_transform(item, &transform);
		localPoint.x = float(local.x);
		localPoint.y = float(local.y);
		localPoint.z = 0.0f;
		vec3_transform(&canvasPoint, &localPoint, &transform);

		return {double(canvasPoint.x), double(canvasPoint.y)};
	}

	static void applyTransformKeepingAnchor(obs_sceneitem_t *item, struct obs_transform_info &info,
						const CanvasPoint &anchorCanvas, const CanvasPoint &anchorLocal)
	{
		for (int i = 0; i < 4; i++) {
			obs_sceneitem_set_info2(item, &info);

			const CanvasPoint mappedAnchor = canvasFromSourceLocal(item, anchorLocal);
			const double dx = anchorCanvas.x - mappedAnchor.x;
			const double dy = anchorCanvas.y - mappedAnchor.y;
			if (std::fabs(dx) < 0.01 && std::fabs(dy) < 0.01) {
				return;
			}

			info.pos.x += float(dx);
			info.pos.y += float(dy);
		}

		obs_sceneitem_set_info2(item, &info);
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

		CanvasPoint local;
		if (!sourceLocalFromCanvas(item, canvas, local)) {
			obs_log(LOG_WARNING, "failed to map preview cursor to scene item space");
			obs_sceneitem_release(item);
			return;
		}
		if (settings.debugLogging) {
			const CanvasPoint mapped = canvasFromSourceLocal(item, local);
			obs_log(LOG_INFO,
				"anchor start: canvas=(%.2f, %.2f) local=(%.2f, %.2f) mapped=(%.2f, %.2f) error=(%.4f, %.4f)",
				canvas.x, canvas.y, local.x, local.y, mapped.x, mapped.y, canvas.x - mapped.x,
				canvas.y - mapped.y);
		}

		const double direction = double(wheelDelta) / 120.0;
		const double speedFactor = std::max(1, settings.speed) / 5.0;
		const double factor = std::pow(1.06, direction * speedFactor);
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
		animation.width = width;
		animation.height = height;
		animation.targetScale.x = float(double(info.scale.x) * ratio);
		animation.targetScale.y = float(double(info.scale.y) * ratio);
		animation.settleFrames = 6;

		if (!timer.isActive()) {
			animationClock.restart();
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

		double elapsedSeconds = 0.016;
		if (animationClock.isValid()) {
			elapsedSeconds = std::clamp(double(animationClock.restart()) / 1000.0, 0.001, 0.050);
		} else {
			animationClock.restart();
		}

		const double duration = std::clamp(settings.smoothness, 0.05, 1.0);
		const double alpha = 1.0 - std::exp(-elapsedSeconds / duration);
		const double currentAbsX = std::max(0.000001, std::fabs(double(info.scale.x)));
		const double targetAbsX = std::max(0.000001, std::fabs(double(animation.targetScale.x)));
		const double currentLogX = std::log(currentAbsX);
		const double targetLogX = std::log(targetAbsX);
		const double desiredLogStep = (targetLogX - currentLogX) * alpha;
		const double maxLogStep = std::log(1.06) * elapsedSeconds / duration;
		const double logStep = std::clamp(desiredLogStep, -maxLogStep, maxLogStep);
		const double nextAbsX = std::exp(currentLogX + logStep);
		const double ratio = nextAbsX / currentAbsX;

		struct vec2 nextScale = {};
		nextScale.x = float(double(info.scale.x) * ratio);
		nextScale.y = float(double(info.scale.y) * ratio);

		const double remainingX = std::fabs(double(animation.targetScale.x) - double(nextScale.x));
		const double remainingY = std::fabs(double(animation.targetScale.y) - double(nextScale.y));
		if (remainingX < 0.0005 && remainingY < 0.0005) {
			nextScale = animation.targetScale;
		}

		info.scale = nextScale;
		applyTransformKeepingAnchor(animation.item, info, animation.anchorCanvas, animation.anchorLocal);
		if (settings.debugLogging) {
			const CanvasPoint mapped = canvasFromSourceLocal(animation.item, animation.anchorLocal);
			obs_log(LOG_INFO,
				"anchor settle: target=(%.2f, %.2f) mapped=(%.2f, %.2f) error=(%.4f, %.4f) scale=(%.6f, %.6f)",
				animation.anchorCanvas.x, animation.anchorCanvas.y, mapped.x, mapped.y,
				animation.anchorCanvas.x - mapped.x, animation.anchorCanvas.y - mapped.y,
				double(nextScale.x), double(nextScale.y));
		}

		if (nextScale.x == animation.targetScale.x && nextScale.y == animation.targetScale.y) {
			if (animation.settleFrames <= 0) {
				stopAnimation();
			} else {
				animation.settleFrames--;
			}
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

		const CanvasPoint localCenter = {double(width) / 2.0, double(height) / 2.0};
		const CanvasPoint canvasCenter = canvasFromSourceLocal(item, localCenter);

		const double signX = info.scale.x < 0.0f ? -1.0 : 1.0;
		const double signY = info.scale.y < 0.0f ? -1.0 : 1.0;
		info.scale.x = float(signX);
		info.scale.y = float(signY);
		applyTransformKeepingAnchor(item, info, canvasCenter, localCenter);
		obs_sceneitem_release(item);
	}

	void stopAnimation()
	{
		timer.stop();
		animationClock.invalidate();
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
		obs_data_set_default_bool(data, "debug_logging", settings.debugLogging);
		obs_data_set_default_int(data, "speed", settings.speed);
		obs_data_set_default_double(data, "smoothness", settings.smoothness);
		obs_data_set_default_double(data, "min_zoom", settings.minZoom);
		obs_data_set_default_double(data, "max_zoom", settings.maxZoom);
		obs_data_set_default_string(data, "mode", settings.mode.c_str());

		settings.enabled = obs_data_get_bool(data, "enabled");
		settings.debugLogging = obs_data_get_bool(data, "debug_logging");
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
		obs_data_set_bool(data, "debug_logging", settings.debugLogging);
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
	QElapsedTimer animationClock;
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
