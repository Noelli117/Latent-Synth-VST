#include "PluginEditor.h"
#include "PluginProcessor.h"
#include <cstdlib>
#include <functional>

namespace {
juce::String findBundledResourceBySuffix(const juce::String &assetSuffix) {
  for (int i = 0; i < BinaryData::namedResourceListSize; ++i) {
    juce::String resourceName(BinaryData::namedResourceList[i]);
    if (resourceName.endsWithIgnoreCase(assetSuffix)) {
      return resourceName;
    }
  }
  return {};
}

juce::String getBundledResourceTextBySuffix(const juce::String &assetSuffix) {
  const juce::String resourceName = findBundledResourceBySuffix(assetSuffix);
  if (resourceName.isEmpty()) {
    return {};
  }

  int dataSize = 0;
  const char *data =
      BinaryData::getNamedResource(resourceName.toRawUTF8(), dataSize);
  if (data == nullptr || dataSize <= 0) {
    return {};
  }

  return juce::String::fromUTF8(data, dataSize);
}

juce::String escapeForInlineScript(juce::String scriptText) {
  // Prevent accidental </script> termination inside embedded JS text.
  scriptText = scriptText.replace("</script", "<\\/script",
                                  true /* ignoreCase */);
  return scriptText;
}

juce::String getUrlParameterValue(const URL &url, const String &name) {
  const auto &names = url.getParameterNames();
  const auto &values = url.getParameterValues();
  const int count = juce::jmin(names.size(), values.size());
  for (int i = 0; i < count; ++i) {
    if (names[i] == name) {
      // Keep query decoding robust across WebView bridges where spaces may be
      // serialized as '+'.
      return URL::removeEscapeChars(values[i].replace("+", " "));
    }
  }
  return {};
}

juce::StringArray getNativeFoldParamIds() {
  return {
      rave_parameters::input_gain,   rave_parameters::channel_mode,
      rave_parameters::input_thresh, rave_parameters::input_ratio,
      rave_parameters::output_gain,  rave_parameters::output_drywet,
      rave_parameters::output_limit, rave_parameters::latency_mode,
      rave_parameters::latent_jitter, rave_parameters::output_width,
      rave_parameters::use_prior,
  };
}

bool isEmergencyNativeUiEnabled() {
  const char *value = std::getenv("LATENT_SYNTH_NATIVE_UI_FALLBACK");
  if (value == nullptr) {
    return false;
  }
  return std::string(value) == "1" || std::string(value) == "true" ||
         std::string(value) == "TRUE";
}

} // namespace

class RaveAPEditor::LatentWebView : public juce::WebBrowserComponent {
public:
  explicit LatentWebView(std::function<void(const String &)> urlHandler)
      : juce::WebBrowserComponent(false), _urlHandler(std::move(urlHandler)) {}

  bool pageAboutToLoad(const String &newURL) override {
    if (newURL.containsIgnoreCase("rave.local/latent") ||
        newURL.startsWithIgnoreCase("rave://latent")) {
      if (_urlHandler) {
        _urlHandler(newURL);
      }
      return false;
    }
    return juce::WebBrowserComponent::pageAboutToLoad(newURL);
  }

private:
  std::function<void(const String &)> _urlHandler;
};

RaveAPEditor::RaveAPEditor(RaveAP &p, AudioProcessorValueTreeState &vts)
    : AudioProcessorEditor(&p), ChangeListener(), _lightLookAndFeel(),
      _darkLookAndFeel(), audioProcessor(p), _avts(vts), _foldablePanel(p),
      _bgFull(ImageCache::getFromMemory(BinaryData::bg_full_png,
                                        BinaryData::bg_full_pngSize)),
      _apiRoot(getApiRoot()) {
  _webOnlyUiMode = !isEmergencyNativeUiEnabled();
  String path =
      juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
          .getFullPathName();

  if (SystemStats::getOperatingSystemType() ==
      SystemStats::OperatingSystemType::MacOSX)
    path += String("/Application Support");
  path += String("/ACIDS/RAVE/");

  _modelsDirPath = File(path);
  if (_modelsDirPath.isDirectory() == false) {
    _modelsDirPath.createDirectory();
  }

  getAvailableModelsFromAPI();

  _header.setLookAndFeel(&_darkLookAndFeel);
  _modelPanel.setLookAndFeel(&_darkLookAndFeel);
  _modelExplorer.setLookAndFeel(&_lightLookAndFeel);
  _foldablePanel.setLookAndFeel(&_lightLookAndFeel);

  _modelExplorer._downloadButton.onClick = [this]() { downloadModelFromAPI(); };
  _modelExplorer._importButton.onClick = [this]() { importModel(); };

  _modelPanel.setSampleRate(p.getSampleRate());

  detectAvailableModels();
  // Model manager button stuff
  _header._modelComboBox.onChange = [this]() {
    String modelPath =
        _availableModelsPaths[_header._modelComboBox.indexOfItemId(
            _header._modelComboBox.getSelectedId())];
    audioProcessor.updateEngine(modelPath.toStdString());
  };

  _header.connectVTS(vts);
  _modelPanel.connectVTS(vts);
  _foldablePanel.connectVTS(vts);

  // link to model
  if (p._rave != nullptr) {
    p._rave->removeChangeListener(this);
    p._rave->addChangeListener(this);
    _modelPanel.setModel(p._rave.get());
  }

  // Model manager button stuff
  _header._modelManagerButton.onClick = [this]() {
    setModelExplorerVisible(_header._modelManagerButton.getToggleState());
  };

  addAndMakeVisible(_header);
  addAndMakeVisible(_modelPanel);
  addAndMakeVisible(_foldablePanel);
  addAndMakeVisible(_console);
  addChildComponent(_modelExplorer);

  setResizable(false, false);
  constexpr int kWebCanvasWidth = 1000;
  constexpr int kWebCanvasHeight = 700;
  constexpr int kLegacyEditorWidth = 1500;
  constexpr int kLegacyEditorHeight = 840;
  constexpr int kWebPaneWidth = 900;
  if (_webOnlyUiMode) {
    getConstrainer()->setMinimumSize(kWebCanvasWidth, kWebCanvasHeight);
    setSize(kWebCanvasWidth, kWebCanvasHeight);
  } else {
    getConstrainer()->setMinimumSize(kLegacyEditorWidth + kWebPaneWidth,
                                     kLegacyEditorHeight);
    setSize(kLegacyEditorWidth + kWebPaneWidth, kLegacyEditorHeight);
  }
  setupLatentWebView();
  resized();
  // startTimer(100.);
}

RaveAPEditor::~RaveAPEditor() {
  // Stop UI callbacks before child components start disappearing.
  _header._modelComboBox.onChange = nullptr;
  _header._modelManagerButton.onClick = nullptr;
  _modelExplorer._downloadButton.onClick = nullptr;
  _modelExplorer._importButton.onClick = nullptr;
  _activeDownloadTask.reset();
  _isFetchingApiModels.store(false);

  _fc.reset();
  _modelPanel.stopTimer();

  if (_latentWebView) {
    _latentWebView->setVisible(false);
    removeChildComponent(_latentWebView.get());
    _latentWebView.reset();
  }

  removeAllChildren();

  if (audioProcessor._rave != nullptr) {
    audioProcessor._rave->removeChangeListener(this);
  }
}

void RaveAPEditor::importModel() {
  juce::Component::SafePointer<RaveAPEditor> safeThis(this);

  _fc.reset(new FileChooser(
      "Choose your model file",
      File::getSpecialLocation(File::SpecialLocationType::userHomeDirectory),
      "*.ts", true));

  _fc->launchAsync(
      FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles,
      [safeThis](const FileChooser &chooser) {
        if (safeThis == nullptr) {
          return;
        }

        auto* self = safeThis.getComponent();
        if (self == nullptr) {
          return;
        }

        File sourceFile;
        auto results = chooser.getURLResults();

        for (auto result : results) {
          if (result.isLocalFile()) {
            sourceFile = result.getLocalFile();
          } else {
            return;
          }
        }
        if (sourceFile.getFileExtension() == ".ts" &&
            sourceFile.getSize() > 0) {
          sourceFile.copyFileTo(self->_modelsDirPath.getNonexistentChildFile(
              sourceFile.getFileName(), ".ts"));
          self->detectAvailableModels();
          self->writeWebModelListScript("");
        }
      });
}

/*
void RaveAPEditor::timerCallback() {
  //_console.setText(String(audioProcessor.getLatencySamples()),
juce::dontSendNotification);
}
*/

void RaveAPEditor::resized() {
  if (_webOnlyUiMode) {
    const auto full = getLocalBounds();
    if (_latentWebView) {
      _latentWebView->setBounds(full);
    }
    _header.setBounds(0, 0, 0, 0);
    _modelPanel.setBounds(0, 0, 0, 0);
    _foldablePanel.setBounds(0, 0, 0, 0);
    _modelExplorer.setBounds(0, 0, 0, 0);
    _console.setBounds(0, 0, 0, 0);
    return;
  }

  // Child components should not handle margins, do it here
  // Setup the header first
  auto contentArea = getLocalBounds().reduced(UI_MARGIN_SIZE);
  _header.setBounds(contentArea.removeFromTop(UI_MARGIN_SIZE * 3));
  contentArea.removeFromTop(UI_MARGIN_SIZE);

  auto leftArea = contentArea;
  if (_latentWebView) {
    // Prefer preserving legacy JUCE layout width; if host gives less total
    // width, preserve a minimum web pane width first.
    const int legacyContentWidth = 1500 - (UI_MARGIN_SIZE * 2);
    const int minWebWidth = 760;
    const int totalWidth = contentArea.getWidth();
    const int leftWidth = (totalWidth >= legacyContentWidth + minWebWidth)
                              ? legacyContentWidth
                              : juce::jmax(900, totalWidth - minWebWidth);
    leftArea = contentArea.removeFromLeft(juce::jlimit(0, totalWidth, leftWidth));

    auto webArea = contentArea;
    webArea.removeFromLeft(UI_MARGIN_SIZE);
    _latentWebView->setBounds(webArea);
  }

  // Now setup the two main bodies (explorer and viz) on left side.
  auto columnWidth = (leftArea.getWidth() - (UI_MARGIN_SIZE * 3)) / 4;
  _modelExplorer.setBounds(leftArea);
  _modelPanel.setBounds(
      leftArea.removeFromLeft(columnWidth * 3 + (UI_MARGIN_SIZE * 2)));
  _foldablePanel.setBounds(leftArea);

  _console.setBounds(0, getLocalBounds().getHeight() - 20, getLocalBounds().getWidth(),
                     20);
}

void RaveAPEditor::paint(juce::Graphics &g) {
  g.fillAll(Colour::fromRGBA(185, 185, 185, 255));
  g.drawImageAt(_bgFull, 0, 0);
}

void RaveAPEditor::log(String /*str*/) {}

void RaveAPEditor::changeListenerCallback(ChangeBroadcaster * /*source*/) {
  if (audioProcessor._rave != nullptr) {
    // std::cout << "set prior in changeListenerCallback to" <<
    // audioProcessor._rave->hasPrior() << std::endl;
    //_modelPanel.setPriorEnabled(audioProcessor._rave->hasPrior());
    _foldablePanel.setBufferSizeRange(
        audioProcessor._rave->getValidBufferSizes());
  }
}

// Directory search functions

bool pathExist(const std::string &s) {
  struct stat buffer;
  return (stat(s.c_str(), &buffer) == 0);
}

std::string capitalizeFirstLetter(std::string text) {
  for (unsigned int x = 0; x < text.length(); x++) {
    if (x == 0) {
      text[x] = toupper(text[x]);
    } else if (text[x - 1] == ' ') {
      text[x] = toupper(text[x]);
    }
  }
  return text;
}

void RaveAPEditor::detectAvailableModels() {
  std::string ext(".ts");
  String previouslySelectedPath;
  const int previousIndex = _header._modelComboBox.getSelectedItemIndex();
  if (previousIndex >= 0 && previousIndex < _availableModelsPaths.size()) {
    previouslySelectedPath = _availableModelsPaths[previousIndex];
  }

  // Reset list of available models
  _availableModelsPaths.clear();
  _availableModels.clear();
  if (!pathExist(_modelsDirPath.getFullPathName().toStdString())) {
    std::cerr << "[-] - Model directory not found" << '\n';
    return;
  }
  try {
    for (auto &p : std::filesystem::recursive_directory_iterator(
             _modelsDirPath.getFullPathName().toStdString())) {
      if (p.path().extension() == ext) {
        _availableModelsPaths.add(String(p.path().string()));
        // Prepare a clean model name for display
        auto tmpModelName = p.path().stem().string();
        std::replace(tmpModelName.begin(), tmpModelName.end(), '_', ' ');
        tmpModelName = capitalizeFirstLetter(tmpModelName);
        _availableModels.add(tmpModelName);
      }
    }
  } catch (const std::filesystem::filesystem_error &e) {
    std::cerr << "[-] - Model not found" << '\n';
    std::cerr << e.what();
  }
  // Reset the comboBox and repopulate with the new detected values
  _header._modelComboBox.clear();
  _header._modelComboBox.addItemList(_availableModels, 1);

  if (_availableModels.isEmpty()) {
    return;
  }

  int selectedIndex = 0;
  if (previouslySelectedPath.isNotEmpty()) {
    const int matchedIndex = _availableModelsPaths.indexOf(previouslySelectedPath);
    if (matchedIndex >= 0) {
      selectedIndex = matchedIndex;
    }
  }

  _header._modelComboBox.setSelectedItemIndex(selectedIndex,
                                              juce::dontSendNotification);
}

bool RaveAPEditor::writeBundledWebAsset(const String &assetSuffix,
                                        const String &outputFileName) {
  const String resourceName = findBundledResourceBySuffix(assetSuffix);
  if (resourceName.isEmpty()) {
    return false;
  }

  int dataSize = 0;
  const char *data =
      BinaryData::getNamedResource(resourceName.toRawUTF8(), dataSize);
  if (data == nullptr || dataSize <= 0) {
    return false;
  }

  const File outputFile = _webUiDir.getChildFile(outputFileName);
  return outputFile.replaceWithData(data, (size_t)dataSize);
}

void RaveAPEditor::mirrorWebDebugFile(const String &fileName,
                                      const String &content) {
  if (_modelsDirPath == File()) {
    return;
  }

  const File debugDir = _modelsDirPath.getChildFile("webui_debug");
  if (!debugDir.exists()) {
    debugDir.createDirectory();
  }

  debugDir.getChildFile(fileName).replaceWithText(content);
}

void RaveAPEditor::setupLatentWebView() {
  _webUiDir = File::getSpecialLocation(File::tempDirectory)
                  .getChildFile("acids-rave-latent-webui");
  if (!_webUiDir.exists()) {
    _webUiDir.createDirectory();
  }

  const bool hasIndex = writeBundledWebAsset("index_html", "index.html");
  const bool hasFlow = writeBundledWebAsset("flowfield_js", "flowfield.js");
  const bool hasStyle = writeBundledWebAsset("style_css", "style.css");
  const bool hasP5 = writeBundledWebAsset("p5_js", "p5.js");
  const bool hasFont = writeBundledWebAsset("cour_ttf", "cour.ttf");
  const bool canLoadUnpackedPage =
      hasIndex && hasFlow && hasStyle && hasP5 && hasFont;

  const String styleText = getBundledResourceTextBySuffix("style_css");
  const String p5Text = getBundledResourceTextBySuffix("p5_js");
  const String flowText = getBundledResourceTextBySuffix("flowfield_js");
  const bool canBuildPackedPage =
      styleText.isNotEmpty() && p5Text.isNotEmpty() && flowText.isNotEmpty();

  _latentWebView = std::make_unique<LatentWebView>(
      [this](const String &url) { handleWebViewUrl(url); });
  addAndMakeVisible(*_latentWebView);

  _header.setVisible(!_webOnlyUiMode);
  _modelPanel.setVisible(!_webOnlyUiMode);
  _foldablePanel.setVisible(!_webOnlyUiMode);
  _console.setVisible(false);
  _modelExplorer.setVisible(false);
  writeWebModelListScript("");
  writeWebNativeFoldParamsScript("");
  writeWebLatentStateScript("");

  if (canLoadUnpackedPage) {
    URL webUiUrl(_webUiDir.getChildFile("index.html"));
    webUiUrl = webUiUrl.withParameter("rave_host", "1");
    webUiUrl = webUiUrl.withParameter("rave_nav_fallback", "1");
    _latentWebView->goToURL(webUiUrl.toString(true));
  } else if (canBuildPackedPage) {
    const String packedHtml =
        "<!DOCTYPE html><html lang='en'><head>"
        "<meta charset='utf-8'/>"
        "<style>" + styleText + "</style>"
        "<script>" + escapeForInlineScript(p5Text) + "</script>"
        "</head><body><main></main>"
        "<script>" + escapeForInlineScript(flowText) + "</script>"
        "</body></html>";
    const File packedFile = _webUiDir.getChildFile("index_packed.html");
    packedFile.replaceWithText(packedHtml);

    URL webUiUrl(packedFile);
    webUiUrl = webUiUrl.withParameter("rave_host", "1");
    webUiUrl = webUiUrl.withParameter("rave_nav_fallback", "0");
    _latentWebView->goToURL(webUiUrl.toString(true));
  } else {
    const String debugHtml =
        "<html><body style='font-family:monospace;background:#111;color:#eee;"
        "padding:16px'>"
        "<h3>Web UI assets failed to initialize</h3>"
        "<p>index=" + String(hasIndex ? "ok" : "fail") + " flow=" +
        String(hasFlow ? "ok" : "fail") + " style=" +
        String(hasStyle ? "ok" : "fail") + " p5=" +
        String(hasP5 ? "ok" : "fail") + " font=" +
        String(hasFont ? "ok" : "fail") + " packed=" +
        String(canBuildPackedPage ? "ok" : "fail") + "</p>"
        "<p>tmp dir: " + _webUiDir.getFullPathName() + "</p>"
        "</body></html>";
    const File debugFile = _webUiDir.getChildFile("debug.html");
    debugFile.replaceWithText(debugHtml);
    _latentWebView->goToURL(URL(debugFile).toString(true));
  }
}

void RaveAPEditor::setModelExplorerVisible(bool showExplorer) {
  if (_webOnlyUiMode) {
    _modelExplorer.setVisible(false);
    return;
  }
  _header._modelManagerButton.setToggleState(showExplorer,
                                             juce::dontSendNotification);
  _header._modelManagerButton.setButtonText(showExplorer ? "Play"
                                                         : "Model Explorer");
  _modelPanel.setVisible(!showExplorer);
  _foldablePanel.setVisible(!showExplorer);
  _modelExplorer.setVisible(showExplorer);
}

void RaveAPEditor::writeWebModelListScript(const String &requestId) {
  if (_webUiDir == File()) {
    return;
  }

  juce::Array<juce::var> localModels;
  for (int i = 0; i < _availableModels.size(); ++i) {
    auto *entry = new DynamicObject();
    entry->setProperty("name", _availableModels[i]);
    entry->setProperty("path",
                       i < _availableModelsPaths.size() ? _availableModelsPaths[i]
                                                        : String());
    localModels.add(juce::var(entry));
  }

  juce::Array<juce::var> onlineModels;
  for (int i = 0; i < _modelExplorer._ApiModelsNames.size(); ++i) {
    onlineModels.add(_modelExplorer._ApiModelsNames[i]);
  }

  auto *payload = new DynamicObject();
  payload->setProperty("req", requestId);
  payload->setProperty("localModels", juce::var(localModels));
  payload->setProperty("onlineModels", juce::var(onlineModels));

  const int selectedIndex = _header._modelComboBox.getSelectedItemIndex();
  if (selectedIndex >= 0 && selectedIndex < _availableModelsPaths.size()) {
    payload->setProperty("selectedPath", _availableModelsPaths[selectedIndex]);
  } else {
    payload->setProperty("selectedPath", "");
  }

  const String json = JSON::toString(juce::var(payload));
  const String script =
      "window.__hostModels = " + json + ";\n"
      "window.__hostModelsVersion = " +
      String(Time::getMillisecondCounterHiRes()) + ";\n";
  _webUiDir.getChildFile("host_models.js").replaceWithText(script);
}

void RaveAPEditor::writeWebNativeFoldParamsScript(const String &requestId) {
  if (_webUiDir == File()) {
    return;
  }

  auto *paramsObj = new DynamicObject();
  const auto paramIds = getNativeFoldParamIds();
  for (int i = 0; i < paramIds.size(); ++i) {
    const auto &id = paramIds[i];
    if (auto *raw = _avts.getRawParameterValue(id)) {
      paramsObj->setProperty(id, raw->load());
    }
  }

  if (auto *latencyRaw = _avts.getRawParameterValue(rave_parameters::latency_mode)) {
    const int latencyExponent = juce::roundToInt(latencyRaw->load());
    paramsObj->setProperty("fft_size", 1 << juce::jlimit(1, 30, latencyExponent));
  }

  auto *payload = new DynamicObject();
  payload->setProperty("req", requestId);
  payload->setProperty("params", juce::var(paramsObj));

  const String json = JSON::toString(juce::var(payload));
  const String script =
      "window.__hostNativeFoldParams = " + json + ";\n"
      "window.__hostNativeFoldParamsVersion = " +
      String(Time::getMillisecondCounterHiRes()) + ";\n";
  _webUiDir.getChildFile("host_native_fold_params.js").replaceWithText(script);
  mirrorWebDebugFile("host_native_fold_params.js", script);
}

void RaveAPEditor::writeWebLatentStateScript(const String &requestId) {
  if (_webUiDir == File()) {
    return;
  }

  juce::Array<juce::var> latentScale;
  juce::Array<juce::var> latentBias;
  juce::Array<juce::var> externalLatent;
  for (size_t i = 0; i < AVAILABLE_DIMS; ++i) {
    latentScale.add(audioProcessor.getLatentScaleValue(i));
    latentBias.add(audioProcessor.getLatentBiasValue(i));
    externalLatent.add(audioProcessor.getExternalLatentValue(i));
  }

  auto *payload = new DynamicObject();
  payload->setProperty("req", requestId);
  payload->setProperty("mode",
                       audioProcessor.getExternalLatentMode() ? "global"
                                                              : "native");
  payload->setProperty("scale", juce::var(latentScale));
  payload->setProperty("bias", juce::var(latentBias));
  payload->setProperty("latent", juce::var(externalLatent));
  payload->setProperty("flowSpeed", audioProcessor.getWebUiFlowSpeed());
  payload->setProperty("flowNoiseScale", audioProcessor.getWebUiFlowNoiseScale());
  payload->setProperty("flowCurve", audioProcessor.getWebUiFlowCurve());
  payload->setProperty("flowGain", audioProcessor.getWebUiFlowGain());
  payload->setProperty("flowContrast", audioProcessor.getWebUiFlowContrast());
  payload->setProperty("flowIntensity", audioProcessor.getWebUiFlowIntensity());
  payload->setProperty("flowRadius", audioProcessor.getWebUiFlowRadius());
  if (auto *latentJitterRaw =
          _avts.getRawParameterValue(rave_parameters::latent_jitter)) {
    payload->setProperty("latentJitter", latentJitterRaw->load());
  }
  if (auto *outputWidthRaw =
          _avts.getRawParameterValue(rave_parameters::output_width)) {
    payload->setProperty("stereoWidth", outputWidthRaw->load());
  }

  const String json = JSON::toString(juce::var(payload));
  const String script =
      "window.__hostLatentState = " + json + ";\n"
      "window.__hostLatentStateVersion = " +
      String(Time::getMillisecondCounterHiRes()) + ";\n";
  _webUiDir.getChildFile("host_latent_state.js").replaceWithText(script);
  mirrorWebDebugFile("host_latent_state.js", script);
}

void RaveAPEditor::handleWebViewUrl(const String &urlString) {
  static const bool verboseBridgeLogs = []() {
    const char *value = std::getenv("LATENT_SYNTH_WEB_BRIDGE_LOG");
    if (value == nullptr) {
      return false;
    }
    const std::string raw(value);
    return raw == "1" || raw == "true" || raw == "TRUE";
  }();

  const auto logBridge = [](const String &message) {
    juce::Logger::writeToLog(message);
    std::cout << message << std::endl;
  };

  URL url(urlString);
  const bool isLegacyBridge =
      url.getDomain().equalsIgnoreCase("rave.local") &&
      url.getSubPath().startsWith("/latent");
  const bool isSchemeBridge = urlString.startsWithIgnoreCase("rave://latent");
  if (!(isLegacyBridge || isSchemeBridge)) {
    return;
  }

  const String action = getUrlParameterValue(url, "action");
  const String requestId = getUrlParameterValue(url, "req");
  const bool isControlAction =
      action.equalsIgnoreCase("model_explorer") ||
      action.equalsIgnoreCase("list_models") ||
      action.equalsIgnoreCase("select_model") ||
      action.equalsIgnoreCase("import_model") ||
      action.equalsIgnoreCase("download_model") ||
      action.equalsIgnoreCase("native_fold_get") ||
      action.equalsIgnoreCase("native_fold_set");
  if (verboseBridgeLogs || isControlAction) {
    logBridge("[latent_synth_v3][web_bridge] action=" + action +
              " req=" + requestId + " url=" + urlString);
  }

  if (action.equalsIgnoreCase("model_explorer")) {
    const String open = getUrlParameterValue(url, "open");
    const bool showExplorer =
        open == "1" || open.equalsIgnoreCase("true");
    logBridge("[latent_synth_v3][web_bridge][model_explorer] "
              "open=" + open +
              " showExplorer=" + String(showExplorer ? 1 : 0));
    setModelExplorerVisible(showExplorer);
    return;
  }
  if (action.equalsIgnoreCase("list_models")) {
    detectAvailableModels();
    if (_modelExplorer._ApiModelsNames.isEmpty()) {
      getAvailableModelsFromAPI();
    }
    logBridge(
        "[latent_synth_v3][web_bridge][list_models] localCount=" +
        String(_availableModels.size()) + " onlineCount=" +
        String(_modelExplorer._ApiModelsNames.size()));
    writeWebModelListScript(requestId);
    return;
  }
  if (action.equalsIgnoreCase("select_model")) {
    const String modelIndexRaw = getUrlParameterValue(url, "index");
    const String modelPath = getUrlParameterValue(url, "path");
    const String modelName = getUrlParameterValue(url, "name");
    logBridge(
        "[latent_synth_v3][web_bridge][select_model] path=" + modelPath +
        " name=" + modelName + " rawIndex=" + modelIndexRaw);

    int indexFromPath = -1;
    int indexFromName = -1;
    int indexFromRaw = -1;

    if (modelPath.isNotEmpty()) {
      indexFromPath = _availableModelsPaths.indexOf(modelPath);
    }
    if (modelName.isNotEmpty()) {
      indexFromName = _availableModels.indexOf(modelName);
    }
    if (modelIndexRaw.isNotEmpty()) {
      indexFromRaw = modelIndexRaw.getIntValue();
      if (indexFromRaw < 0 || indexFromRaw >= _availableModelsPaths.size()) {
        indexFromRaw = -1;
      }
    }

    int matchIndex = -1;
    if (indexFromPath >= 0) {
      matchIndex = indexFromPath;
    } else if (indexFromName >= 0) {
      matchIndex = indexFromName;
    } else {
      matchIndex = indexFromRaw;
    }

    logBridge(
        "[latent_synth_v3][web_bridge][select_model] indexFromPath=" +
        String(indexFromPath) + " indexFromName=" + String(indexFromName) +
        " indexFromRaw=" + String(indexFromRaw) +
        " matchIndex=" + String(matchIndex));

    if (matchIndex >= 0) {
      String resolvedName;
      String resolvedPath;
      if (matchIndex < _availableModels.size()) {
        resolvedName = _availableModels[matchIndex];
      }
      if (matchIndex < _availableModelsPaths.size()) {
        resolvedPath = _availableModelsPaths[matchIndex];
      }
      logBridge(
          "[latent_synth_v3][web_bridge][select_model] selecting name=" +
          resolvedName + " path=" + resolvedPath);
      _header._modelComboBox.setSelectedItemIndex(matchIndex, true);
    } else {
      logBridge(
          "[latent_synth_v3][web_bridge][select_model] no match found, "
          "keeping current selection");
    }
    writeWebModelListScript(requestId);
    return;
  }
  if (action.equalsIgnoreCase("import_model")) {
    juce::Component::SafePointer<RaveAPEditor> safeThis(this);
    juce::MessageManager::callAsync([safeThis]() {
      if (safeThis != nullptr) {
        if (auto* self = safeThis.getComponent()) {
          self->importModel();
        }
      }
    });
    return;
  }
  if (action.equalsIgnoreCase("download_model")) {
    const String modelName = getUrlParameterValue(url, "name");
    downloadModelByName(modelName);
    detectAvailableModels();
    writeWebModelListScript(requestId);
    return;
  }
  if (action.equalsIgnoreCase("native_fold_get")) {
    writeWebNativeFoldParamsScript(requestId);
    writeWebLatentStateScript(requestId);
    return;
  }
  if (action.equalsIgnoreCase("native_fold_set")) {
    const String paramId = getUrlParameterValue(url, "id");
    const String rawValue = getUrlParameterValue(url, "value");
    if (paramId.isNotEmpty() && rawValue.isNotEmpty()) {
      if (auto *param = _avts.getParameter(paramId)) {
        const auto range = param->getNormalisableRange();
        const float value =
            juce::jlimit(range.start, range.end, rawValue.getFloatValue());
        param->beginChangeGesture();
        param->setValueNotifyingHost(range.convertTo0to1(value));
        param->endChangeGesture();
      }
    }
    writeWebNativeFoldParamsScript(requestId);
    writeWebLatentStateScript(requestId);
    return;
  }

  const String seqRaw = getUrlParameterValue(url, "seq");
  if (seqRaw.isNotEmpty()) {
    const int64 seq = seqRaw.getLargeIntValue();
    if (_lastLatentSeq >= 0) {
      if (seq <= _lastLatentSeq) {
        if (verboseBridgeLogs) {
          logBridge("[latent_synth_v3][web_bridge][latent_stream] non-monotonic "
                    "seq=" + String(seq) + " lastSeq=" + String(_lastLatentSeq));
        }
      } else if (seq > _lastLatentSeq + 1) {
        const int64 dropped = seq - (_lastLatentSeq + 1);
        _droppedLatentPackets += dropped;
        logBridge("[latent_synth_v3][web_bridge][latent_stream] dropped=" +
                  String(dropped) + " totalDropped=" +
                  String(_droppedLatentPackets) + " lastSeq=" +
                  String(_lastLatentSeq) + " currentSeq=" + String(seq));
      }
    }
    if (seq > _lastLatentSeq) {
      _lastLatentSeq = seq;
    }
  }

  const String mode = getUrlParameterValue(url, "mode");
  const bool useGlobalLatent =
      mode.equalsIgnoreCase("global") || mode.equalsIgnoreCase("ui");
  if (mode.isNotEmpty()) {
    audioProcessor.setExternalLatentMode(useGlobalLatent);
  }

  const String rawFlowSpeed = getUrlParameterValue(url, "fspeed");
  const String rawFlowNoiseScale = getUrlParameterValue(url, "fnoise");
  const String rawFlowCurve = getUrlParameterValue(url, "fcurve");
  const String rawFlowGain = getUrlParameterValue(url, "fgain");
  const String rawFlowContrast = getUrlParameterValue(url, "fcontrast");
  const String rawFlowIntensity = getUrlParameterValue(url, "fintensity");

  if (rawFlowSpeed.isNotEmpty()) {
    audioProcessor.setWebUiFlowSpeed(rawFlowSpeed.getFloatValue());
  }
  if (rawFlowNoiseScale.isNotEmpty()) {
    audioProcessor.setWebUiFlowNoiseScale(rawFlowNoiseScale.getFloatValue());
  }
  if (rawFlowCurve.isNotEmpty()) {
    audioProcessor.setWebUiFlowCurve(rawFlowCurve.getFloatValue());
  }
  if (rawFlowGain.isNotEmpty()) {
    audioProcessor.setWebUiFlowGain(rawFlowGain.getFloatValue());
  }
  if (rawFlowContrast.isNotEmpty()) {
    audioProcessor.setWebUiFlowContrast(rawFlowContrast.getFloatValue());
  }
  if (rawFlowIntensity.isNotEmpty()) {
    audioProcessor.setWebUiFlowIntensity(rawFlowIntensity.getFloatValue());
  }

  for (size_t i = 0; i < AVAILABLE_DIMS; ++i) {
    const String scaleName = "s" + String((int)i);
    const String biasName = "b" + String((int)i);

    const String rawScale = getUrlParameterValue(url, scaleName);
    const String rawBias = getUrlParameterValue(url, biasName);

    if (rawScale.isNotEmpty()) {
      audioProcessor.setLatentScaleValue(i, rawScale.getFloatValue());
    }
    if (rawBias.isNotEmpty()) {
      audioProcessor.setLatentBiasValue(i, rawBias.getFloatValue());
    }
  }

  writeWebLatentStateScript(requestId);
}
