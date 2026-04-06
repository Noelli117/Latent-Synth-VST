#include "PluginEditor.h"
#include <thread>

void RaveAPEditor::finished(URL::DownloadTask *task, bool success) {
  juce::Component::SafePointer<RaveAPEditor> safeThis(this);
  juce::MessageManager::callAsync([safeThis, task, success]() {
    if (safeThis == nullptr) {
      return;
    }
    auto *self = safeThis.getComponent();
    if (self == nullptr) {
      return;
    }

    if (self->_activeDownloadTask.get() == task) {
      self->_activeDownloadTask.reset();
    }

    if (success) {
      std::cout << "[+] Network - Model downloaded" << std::endl;
      self->detectAvailableModels();
      self->writeWebModelListScript("");
    } else {
      std::cerr << "[-] Network - Failed to download model" << std::endl;
    }
  });
}

void RaveAPEditor::progress(URL::DownloadTask *task, int64 bytesDownloaded,
                            int64 totalLength) {
  juce::ignoreUnused(task);
  std::cout << bytesDownloaded << "/" << totalLength << '\n';
}

bool RaveAPEditor::downloadModelByName(const String &modelName) {
  if (modelName.isEmpty()) {
    std::cout << "[ ] Network - Empty model name" << std::endl;
    return false;
  }
  if (_activeDownloadTask && !_activeDownloadTask->isFinished()) {
    std::cout << "[ ] Network - Download already in progress" << std::endl;
    return false;
  }
  if (_modelExplorer._ApiModelsData.size() < 1) {
    std::cout << "[ ] Network - No models available for download" << std::endl;
    // AlertWindow::showAsync(MessageBoxOptions()
    //                            .withIconType(MessageBoxIconType::WarningIcon)
    //                            .withTitle("Information:")
    //                            .withMessage("No models available for
    //                            download") .withButton("OK"),
    //                        nullptr);
    return false;
  }
  String tmp_url = _apiRoot + String("get_model?model_name=") +
                   URL::addEscapeChars(modelName, true, false);
  URL url = URL(tmp_url);

  std::cout << url.toString(true) << '\n';

  // TODO: Clean up the path handling, avoid using String concatenation for
  // that
  String outputFilePath = _modelsDirPath.getFullPathName() + String("/") +
                          modelName + String(".ts");
  File outputFile = File(outputFilePath);
  auto task = url.downloadToFile(
      outputFile, URL::DownloadTaskOptions().withListener(this));

  if (task == nullptr) {
    std::cerr << "[-] Network - Failed to start download task" << std::endl;
    return false;
  }
  if (task->hadError()) {
    std::cerr << "[-] Network - Failed to download model" << std::endl;
    // AlertWindow::showAsync(MessageBoxOptions()
    //                            .withIconType(MessageBoxIconType::WarningIcon)
    //                            .withTitle("Network Warning:")
    //                            .withMessage("Failed to download model")
    //                            .withButton("OK"),
    //                        nullptr);
    return false;
  }

  _activeDownloadTask = std::move(task);
  return true;
}

void RaveAPEditor::downloadModelFromAPI() {
  if (_modelExplorer._ApiModelsData.size() < 1) {
    std::cout << "[ ] Network - No models available for download" << std::endl;
    return;
  }
  const int selectedRow = _modelExplorer._modelsList.getSelectedRow();
  if (selectedRow < 0 || selectedRow >= _modelExplorer._ApiModelsNames.size()) {
    std::cout << "[ ] Network - No model selected for download" << std::endl;
    return;
  }
  downloadModelByName(_modelExplorer._ApiModelsNames[selectedRow]);
}

void RaveAPEditor::getAvailableModelsFromAPI() {
  if (_isFetchingApiModels.exchange(true)) {
    return;
  }

  juce::Component::SafePointer<RaveAPEditor> safeThis(this);
  const String apiRoot = _apiRoot;

  std::thread([safeThis, apiRoot]() {
    URL url(apiRoot + String("get_available_models"));
    String res = url.readEntireTextStream();

    if (juce::MessageManager::getInstanceWithoutCreating() == nullptr) {
      return;
    }

    juce::MessageManager::callAsync([safeThis, res]() {
      if (safeThis == nullptr) {
        return;
      }
      auto *self = safeThis.getComponent();
      if (self == nullptr) {
        return;
      }

      self->_isFetchingApiModels.store(false);

      if (res.isEmpty()) {
        std::cerr << "[-] Network - No API response" << std::endl;
        return;
      }

      std::cout << "[ ] Network - Received API response" << std::endl;

      juce::var parsedJson;
      if (!JSON::parse(res, parsedJson).wasOk()) {
        std::cerr << "[-] Network - Failed to parse API response" << std::endl;
        return;
      }

      auto *availableModels = parsedJson["available_models"].getArray();
      if (availableModels == nullptr) {
        std::cerr << "[-] Network - Missing available_models in API response"
                  << std::endl;
        return;
      }

      self->_parsedJson = parsedJson;
      self->_modelExplorer._ApiModelsNames.clear();
      self->_modelExplorer._ApiModelsData.clear();

      std::cout << "[+] Network - Successfully parsed JSON, "
                << availableModels->size() << " models available online:"
                << '\n';

      for (int i = 0; i < availableModels->size(); i++) {
        const String modelName = (*availableModels)[i].toString();
        std::cout << "\t- " << modelName << '\n';
        self->_modelExplorer._ApiModelsNames.add(modelName);

        auto modelVar = parsedJson[modelName.toStdString().c_str()];
        if (auto *modelObj = modelVar.getDynamicObject()) {
          self->_modelExplorer._ApiModelsData.add(modelObj->getProperties());
        } else {
          self->_modelExplorer._ApiModelsData.add(juce::NamedValueSet());
        }
      }

      self->_modelExplorer._modelsList.updateContent();
      self->writeWebModelListScript("");
    });
  }).detach();
}

String RaveAPEditor::getApiRoot() {
  unsigned char b[] = {104, 116, 116, 112, 115, 58,  47,  47, 112, 108, 97,
                       121, 46,  102, 111, 114, 117, 109, 46, 105, 114, 99,
                       97,  109, 46,  102, 114, 47,  114, 97, 118, 101, 45,
                       118, 115, 116, 45,  97,  112, 105, 47};
  char c[sizeof(b) + 1];
  memcpy(c, b, sizeof(b));
  c[sizeof(b)] = '\0';
  return String(c);
}
