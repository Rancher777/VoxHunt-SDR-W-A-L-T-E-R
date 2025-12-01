#include <imgui.h>
#include <module.h>
#include <gui/gui.h>
#include <vector>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <fstream>
#include <unistd.h> // For chdir
#include <signal.h> // For kill
#include <signal_path/signal_path.h>
#include <gui/widgets/waterfall.h>
#include <dsp/sink/handler_sink.h>
#include <dsp/routing/splitter.h>
#include <dsp/multirate/rational_resampler.h>
#include <dsp/convert/stereo_to_mono.h>
#include <utils/proto/http.h>
#include <config.h>
#include "whisper.h"
#include <core.h>
#include <sys/wait.h> // For waitpid
#include <fcntl.h>    // For open
#include <limits.h>   // For PATH_MAX
#include <stdlib.h>   // For realpath

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Whisper sample rate is always 16000 Hz
#define WHISPER_SAMPLE_RATE 16000

SDRPP_MOD_INFO{
    /* Name:            */ "SIGINT AI",
    /* Description:     */ "AI-powered SIGINT module",
    /* Author:          */ "Gemini & Rancher777",
    /* Version:         */ 0, 1, 0,
    /* Max instances    */ 1
};

class AtakSigintModule : public ModuleManager::Instance {
public:
    AtakSigintModule(std::string name) {
        this->name = name;
        gui::menu.registerEntry(name, menuHandler, this, NULL);
        logMessages.push_back("ATAK SIGINT Module Initialized.");
        
        // Set pop-out log window to be shown by default
        showLogWindow = true;
    }

    ~AtakSigintModule() {
        stopWhisperWorker = true;
        if (whisperWorker.joinable()) {
            whisperWorker.join();
        }
        stopOllamaMonitor = true;
        if (ollamaMonitorThread.joinable()) {
            ollamaMonitorThread.join();
        }

        gui::menu.removeEntry(name);
        if (audioStream) {
            sigpath::sinkManager.unbindStream(selectedStreamName, audioStream);
        }
        splitter.stop();
        splitter.unbindStream(&splitterOutput);
        stereoToMono.stop();
        resampler.stop();
        if (whisperCtx) {
            whisper_free(whisperCtx);
        }
    }

    void postInit() {
        // Determine path of model relative to the executable
        std::string modelPath = "";
        char exePath[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len != -1) {
            exePath[len] = '\0';
            std::string exeDir = std::string(exePath);
            size_t lastSlash = exeDir.find_last_of("/");
            if (lastSlash != std::string::npos) {
                exeDir = exeDir.substr(0, lastSlash);
            }
            modelPath = exeDir + "/ggml-tiny.en.bin";
        } else {
            logMessages.push_back("[ERROR] Could not determine executable path. Cannot load Whisper model.");
            return;
        }
        
        logMessages.push_back("Loading Whisper model from: " + modelPath);
        whisperCtx = whisper_init_from_file(modelPath.c_str());
        if (whisperCtx) {
            logMessages.push_back("Whisper model loaded successfully.");
        } else {
            logMessages.push_back("Error: Failed to load Whisper model.");
            return;
        }

        logMessages.push_back("Attempting to bind to '" + selectedStreamName + "' audio stream...");
        audioStream = sigpath::sinkManager.bindStream(selectedStreamName);
        if (audioStream) {
            // Initialize the splitter with the main audio stream
            splitter.init(audioStream);
            splitter.start();
            // Bind our output stream to the splitter
            splitter.bindStream(&splitterOutput);

            // Convert stereo to mono float
            stereoToMono.init(&splitterOutput);
            stereoToMono.start();

            // Initialize resampler (assuming 48kHz input from SDR++, 16kHz for Whisper)
            resampler.init(&stereoToMono.out, 48000.0f, (float)WHISPER_SAMPLE_RATE);
            resampler.start();

            // Initialize our audio sink with the resampler's output stream
            audioSink.init(&resampler.out, audioHandler, this);
            audioSink.start();
            logMessages.push_back("Successfully bound to audio stream via splitter, stereo-to-mono, and resampler.");
        } else {
            logMessages.push_back("Error: Could not bind to audio stream. Is Radio running?");
        }

        ollamaMonitorThread = std::thread(&AtakSigintModule::ollamaMonitorLoop, this);
        whisperWorker = std::thread(&AtakSigintModule::whisperWorkerLoop, this);
    }

    void enable() { enabled = true; }
    void disable() { enabled = false; }
    bool isEnabled() { return enabled; }

private:
    static void menuHandler(void* ctx) {
        AtakSigintModule* _this = (AtakSigintModule*)ctx;
        _this->draw();
    }

    static void audioHandler(float* data, int count, void* ctx) {
        AtakSigintModule* _this = (AtakSigintModule*)ctx;
        if (!_this->voiceHuntActive) { return; }
        std::lock_guard<std::mutex> lock(_this->audioBufferMutex);
        for (int i = 0; i < count; i++) {
            _this->audioBuffer.push_back(data[i]);
        }
    }

    void checkOllamaStatus() {
        // Use lsof and grep for LISTEN state. This is the most reliable check.
        // It ensures a process is actively listening on the port.
        int result = system("lsof -i :11434 | grep LISTEN > /dev/null 2>&1");

        bool is_running = (WIFEXITED(result) && WEXITSTATUS(result) == 0);

        if (is_running && !ollamaRunning) {
            ollamaRunning = true;
            std::lock_guard<std::mutex> lock(logMutex);
            logMessages.push_back("[OLLAMA Status Check] Ollama detected as running.");
        } else if (!is_running && ollamaRunning) {
            ollamaRunning = false;
            std::lock_guard<std::mutex> lock(logMutex);
            logMessages.push_back("[OLLAMA Status Check] Ollama not detected as running.");
        }
    }



    void ollamaMonitorLoop() {
        while (!stopOllamaMonitor) {
            checkOllamaStatus();
            if (ollamaRunning && !modelsLoaded) {
                fetchOllamaModels();
            }
            std::this_thread::sleep_for(std::chrono::seconds(1)); // Check every 1 second
        }
    }

    void fetchOllamaModels() {
        net::http::Client httpClient;
        std::string ollamaResponse;
        try {
            ollamaResponse = httpClient.get("http://localhost:11434/api/tags");
            std::lock_guard<std::mutex> lock(logMutex); // Lock for logging
            logMessages.push_back("[OLLAMA] Raw API response: " + ollamaResponse); // Log raw response for debugging
            
            if (ollamaResponse.empty()) {
                logMessages.push_back("[OLLAMA Error] Empty response from Ollama API. Is the server running?");
                modelsLoaded = false;
                return;
            }

            json responseJson = json::parse(ollamaResponse);
            if (responseJson.contains("models") && responseJson["models"].is_array()) {
                logMessages.push_back("[OLLAMA] Detected models:");
                availableModels.clear(); // Clear previous models
                for (const auto& model : responseJson["models"]) {
                    if (model.contains("name") && model["name"].is_string()) {
                        availableModels.push_back(model["name"].get<std::string>());
                        logMessages.push_back("  - " + model["name"].get<std::string>());
                    }
                    // Check if the default model "llama3:8b" is available and select it
                    if (model["name"].get<std::string>() == "llama3:8b" && selectedModelIndex == 0) {
                        selectedModelIndex = availableModels.size() - 1; // Select this model if it's the default and not already set
                    }
                }
                if (!availableModels.empty()) {
                    modelsLoaded = true;
                } else {
                    logMessages.push_back("[OLLAMA] No models found.");
                    modelsLoaded = false;
                }
            } else {
                logMessages.push_back("[OLLAMA Error] API response did not contain 'models' array or was malformed.");
                modelsLoaded = false;
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(logMutex);
            logMessages.push_back("[OLLAMA Error] Failed to fetch models: " + std::string(e.what()));
            if (!ollamaResponse.empty()) {
                logMessages.push_back("[OLLAMA Error] Response that caused error: " + ollamaResponse);
            }
            modelsLoaded = false;
        }
    }

    void whisperWorkerLoop() {
        while (!stopWhisperWorker) {
            std::vector<float> pcm32f;
            {
                std::lock_guard<std::mutex> lock(audioBufferMutex);
                if (audioBuffer.size() > WHISPER_SAMPLE_RATE * 5) { // Process ~5 seconds of 16kHz audio
                    pcm32f = audioBuffer;
                    audioBuffer.clear();
                }
            }

            if (!pcm32f.empty()) {
                if (!whisperCtx) { continue; }
                whisper_full_params params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                params.print_progress = false;
                params.print_special = false;
                params.print_timestamps = false;
                params.print_realtime = false;
                params.translate = false;
                params.language = "en";
                params.n_threads = 4;

                if (whisper_full(whisperCtx, params, pcm32f.data(), pcm32f.size()) == 0) {
                    int n_segments = whisper_full_n_segments(whisperCtx);
                    std::string transcript = "";
                    for (int i = 0; i < n_segments; ++i) {
                        transcript += whisper_full_get_segment_text(whisperCtx, i);
                    }

                    if (transcript.length() > 1) {
                        std::lock_guard<std::mutex> lock(logMutex);
                        logMessages.push_back("[WHISPER] " + transcript);

                        // Ollama Integration
                        if (atakAiActive && ollamaRunning && modelsLoaded && !availableModels.empty()) {
                            // Lazy initialize Ollama messages with a system prompt for context
                            if (!ollamaInitialized) {
                                ollamaMessages.push_back(json::parse(R"({"role": "system", "content": "You are a U.S. Navy S.E.A.L. on a covert SIGINT operation. Your callsign is RADAR. Be brief and professional. Report only significant, actionable intelligence. Otherwise, learn from the OPERATOR's instructions. When responding to the OPERATOR, be concise. End all transmissions with OVER."})"));
                                ollamaInitialized = true;
                            }

                            // Add current transcription to Ollama messages
                            json userMessage;
                            userMessage["role"] = "user";
                            userMessage["content"] = "Intercepted Transmission (HEARD): \"" + transcript + "\"";
                            ollamaMessages.push_back(userMessage);

                            // Limit history length
                            while (ollamaMessages.size() > MAX_HISTORY_LENGTH) {
                                ollamaMessages.erase(ollamaMessages.begin() + 1); // Keep system prompt, remove oldest user/assistant
                            }

                            json ollamaPayload;
                            ollamaPayload["model"] = availableModels[selectedModelIndex]; // Use selected model
                            ollamaPayload["messages"] = ollamaMessages; // Send the entire message history
                            ollamaPayload["stream"] = false;
                            ollamaPayload["options"]["temperature"] = 0.4;
                            ollamaPayload["options"]["num_predict"] = 80;

                            net::http::Client httpClient;
                            try {
                                std::string ollamaResponse = httpClient.post("http://localhost:11434/api/chat", ollamaPayload.dump()); // Use /api/chat for messages array
                                json responseJson = json::parse(ollamaResponse);
                                std::string aiText = responseJson["message"]["content"].get<std::string>();
                                logMessages.push_back("[RADAR] " + aiText);

                                // Add AI response to Ollama messages
                                json assistantMessage;
                                assistantMessage["role"] = "assistant";
                                assistantMessage["content"] = aiText;
                                ollamaMessages.push_back(assistantMessage);
                            } catch (const std::exception& e) {
                                logMessages.push_back("[AI Error] HTTP or JSON error: " + std::string(e.what()));
                            }
                        }
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }

    void draw() {
        // Prevent scroll events from leaking to the main window
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) || ImGui::IsAnyItemHovered()) {
            ImGui::GetIO().WantCaptureMouse = true;
        }

        {
            std::lock_guard<std::mutex> lock(logMutex);
            if (lastLogSize != logMessages.size()) {
                // Open the log file in append mode
                std::ofstream logFile("/tmp/atak_sigint.log", std::ios_base::app);
                if (logFile.is_open()) {
                    // Write only the new messages
                    for (size_t i = lastLogSize; i < logMessages.size(); ++i) {
                        logFile << logMessages[i] << std::endl;
                    }
                }
                lastLogSize = logMessages.size();
                scrollToBottom = true;
            }
        }

        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4, 8));

        // Pop-out Log button in main panel
        if (ImGui::Button("Pop-out Log")) {
            showLogWindow = !showLogWindow;
        }

        ImGui::Separator();

        // Ollama Control
        ImGui::Text("Ollama Server Status: %s", ollamaRunning ? "Running" : "Not Running");
        if (isWarmingModel) {
            ImGui::Text("%s", warmingStatusMessage.c_str());
        }
        ImGui::Separator();

        // Ollama Model Selection
        ImGui::BeginDisabled(isWarmingModel);
        if (ollamaRunning && modelsLoaded && !availableModels.empty()) {
            ImGui::Text("AI Model"); ImGui::SameLine();
            ImGui::PushItemWidth(-1);
            if (ImGui::BeginCombo("##ollama_model_select", availableModels[selectedModelIndex].c_str())) {
                for (int i = 0; i < availableModels.size(); ++i) {
                    const bool is_selected = (selectedModelIndex == i);
                    if (ImGui::Selectable(availableModels[i].c_str(), is_selected)) {
                        if (selectedModelIndex != i) {
                            int oldIndex = selectedModelIndex;
                            selectedModelIndex = i;
                            std::thread(&AtakSigintModule::warmupModel, this, i, oldIndex).detach();
                        }
                    }
                    if (is_selected) {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
        } else if (ollamaRunning && !modelsLoaded) {
            ImGui::Text("Loading Ollama models...");
        } else {
            ImGui::Text("Ollama not running. Start server to select models.");
        }
        ImGui::EndDisabled();

        // Embedded Log Window (only visible if not popped out)
        if (!showLogWindow) {
            ImGui::Text("SIGINT LOG");
            ImGui::BeginChild("LogWindow", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true, ImGuiWindowFlags_HorizontalScrollbar);
            {
                std::lock_guard<std::mutex> lock(logMutex);
                for (const auto& msg : logMessages) {
                    ImGui::TextUnformatted(msg.c_str());
                }
            }
            if (scrollToBottom) {
                ImGui::SetScrollHereY(1.0f);
                scrollToBottom = false;
            }
            ImGui::EndChild();

            ImGui::Separator();

            ImGui::BeginDisabled(isWarmingModel);
            ImGui::PushItemWidth(-150);
            if (ImGui::InputText("##chat", chatInputBuffer, sizeof(chatInputBuffer), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::Button("Send", ImVec2(140, 0))) {
                if (strlen(chatInputBuffer) > 0) {
                    // Copy input to a separate string so we can clear the buffer
                    std::string message = chatInputBuffer;
                    memset(chatInputBuffer, 0, sizeof(chatInputBuffer));

                    // All network and processing happens outside of any locks
                    std::string ollamaResponse;
                    std::string aiText;
                    std::string errorMessage;
                    bool success = false;

                    if (atakAiActive && ollamaRunning && modelsLoaded && !availableModels.empty()) {
                        // Prepare payload
                        json ollamaPayload;
                        json userMessage;
                        userMessage["role"] = "user";
                        userMessage["content"] = message;
                        
                        auto tempMessages = ollamaMessages;
                        tempMessages.push_back(userMessage);

                        if (!ollamaInitialized) {
                            tempMessages.insert(tempMessages.begin(), json::parse(R"({"role": "system", "content": "You are a U.S. Navy S.E.A.L. on a covert SIGINT operation. Your callsign is RADAR. Be brief and professional. Report only significant, actionable intelligence. Otherwise, learn from the OPERATOR's instructions. When responding to the OPERATOR, be concise. End all transmissions with OVER."})"));
                            ollamaInitialized = true;
                        }

                        while (tempMessages.size() > MAX_HISTORY_LENGTH) {
                            tempMessages.erase(tempMessages.begin() + 1);
                        }

                        ollamaPayload["model"] = availableModels[selectedModelIndex];
                        ollamaPayload["messages"] = tempMessages;
                        ollamaPayload["stream"] = false;
                        ollamaPayload["options"]["temperature"] = 0.4;
                        ollamaPayload["options"]["num_predict"] = 80;

                        // Perform network request
                        net::http::Client httpClient;
                        try {
                            ollamaResponse = httpClient.post("http://localhost:11434/api/chat", ollamaPayload.dump());
                            json responseJson = json::parse(ollamaResponse);
                            aiText = responseJson["message"]["content"].get<std::string>();
                            success = true;
                        } catch (const std::exception& e) {
                            errorMessage = "[AI Error] HTTP or JSON error: " + std::string(e.what());
                            success = false;
                        }
                    }

                    // Now, acquire a single lock to update all logs and state
                    {
                        std::lock_guard<std::mutex> lock(logMutex);
                        logMessages.push_back("OPERATOR: " + message);
                        if (success) {
                            logMessages.push_back("[AI Raw Response] " + ollamaResponse);
                            logMessages.push_back("[AI] " + aiText);

                            json userMsgJson;
                            userMsgJson["role"] = "user";
                            userMsgJson["content"] = message;
                            ollamaMessages.push_back(userMsgJson);

                            json assistantMsgJson;
                            assistantMsgJson["role"] = "assistant";
                            assistantMsgJson["content"] = aiText;
                            ollamaMessages.push_back(assistantMsgJson);

                            while (ollamaMessages.size() > MAX_HISTORY_LENGTH) {
                                ollamaMessages.erase(ollamaMessages.begin() + 1);
                            }
                        } else if (!errorMessage.empty()) {
                            logMessages.push_back(errorMessage);
                        }
                        scrollToBottom = true;
                    }
                }
            }
            ImGui::PopItemWidth();
            ImGui::EndDisabled();
        }
        ImGui::PopStyleVar();

        // Pop-out Log Window
        if (showLogWindow) {
            if (ImGui::Begin("SIGINT LOG", &showLogWindow)) {
                // Checkboxes inside the pop-out window
                ImGui::Checkbox("VoxHunt", &voiceHuntActive);
                ImGui::SameLine();
                ImGui::Checkbox("W*A*L*T*E*R", &atakAiActive);
                ImGui::Separator();

                // Ollama Control in pop-out window
                ImGui::Text("Ollama Server Status: %s", ollamaRunning ? "Running" : "Not Running");
                if (isWarmingModel) {
                    ImGui::Text("%s", warmingStatusMessage.c_str());
                }
                ImGui::Separator();

                // Ollama Model Selection in pop-out window
                ImGui::BeginDisabled(isWarmingModel);
                if (ollamaRunning && modelsLoaded && !availableModels.empty()) {
                    ImGui::Text("AI Model"); ImGui::SameLine();
                    ImGui::PushItemWidth(-1);
                    if (ImGui::BeginCombo("##ollama_model_select_popout", availableModels[selectedModelIndex].c_str())) {
                        for (int i = 0; i < availableModels.size(); ++i) {
                            const bool is_selected = (selectedModelIndex == i);
                            if (ImGui::Selectable(availableModels[i].c_str(), is_selected)) {
                                if (selectedModelIndex != i) {
                                    int oldIndex = selectedModelIndex;
                                    selectedModelIndex = i;
                                    std::thread(&AtakSigintModule::warmupModel, this, i, oldIndex).detach();
                                }
                            }
                            if (is_selected) {
                                ImGui::SetItemDefaultFocus();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();
                } else if (ollamaRunning && !modelsLoaded) {
                    ImGui::Text("Loading Ollama models...");
                } else {
                    ImGui::Text("Ollama not running. Start server to select models.");
                }
                ImGui::EndDisabled();
                ImGui::Separator();

                ImGui::BeginChild("PopOutLogWindow", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true, ImGuiWindowFlags_HorizontalScrollbar);
                {
                    std::lock_guard<std::mutex> lock(logMutex);
                    for (const auto& msg : logMessages) {
                        ImGui::TextUnformatted(msg.c_str());
                    }
                }
                if (scrollToBottom) {
                    ImGui::SetScrollHereY(1.0f);
                    scrollToBottom = false;
                }
                ImGui::EndChild();

                ImGui::Separator();

                ImGui::BeginDisabled(isWarmingModel);
                ImGui::PushItemWidth(-150);
                if (ImGui::InputText("##chatPopOut", chatInputBuffer, sizeof(chatInputBuffer), ImGuiInputTextFlags_EnterReturnsTrue) || ImGui::Button("Send", ImVec2(140, 0))) {
                    if (strlen(chatInputBuffer) > 0) {
                        // Copy input to a separate string so we can clear the buffer
                        std::string message = chatInputBuffer;
                        memset(chatInputBuffer, 0, sizeof(chatInputBuffer));

                        // All network and processing happens outside of any locks
                        std::string ollamaResponse;
                        std::string aiText;
                        std::string errorMessage;
                        bool success = false;

                        if (atakAiActive && ollamaRunning && modelsLoaded && !availableModels.empty()) {
                            // Prepare payload
                            json ollamaPayload;
                            json userMessage;
                            userMessage["role"] = "user";
                            userMessage["content"] = message;
                            
                            auto tempMessages = ollamaMessages;
                            tempMessages.push_back(userMessage);

                            if (!ollamaInitialized) {
                                tempMessages.insert(tempMessages.begin(), json::parse(R"({"role": "system", "content": "You are a U.S. Navy S.E.A.L. on a covert SIGINT operation. Your callsign is RADAR. Be brief and professional. Report only significant, actionable intelligence. Otherwise, learn from the OPERATOR's instructions. When responding to the OPERATOR, be concise. End all transmissions with OVER."})"));
                                ollamaInitialized = true;
                            }

                            while (tempMessages.size() > MAX_HISTORY_LENGTH) {
                                tempMessages.erase(tempMessages.begin() + 1);
                            }

                            ollamaPayload["model"] = availableModels[selectedModelIndex];
                            ollamaPayload["messages"] = tempMessages;
                            ollamaPayload["stream"] = false;
                            ollamaPayload["options"]["temperature"] = 0.4;
                            ollamaPayload["options"]["num_predict"] = 80;

                            // Perform network request
                            net::http::Client httpClient;
                            try {
                                ollamaResponse = httpClient.post("http://localhost:11434/api/chat", ollamaPayload.dump());
                                json responseJson = json::parse(ollamaResponse);
                                aiText = responseJson["message"]["content"].get<std::string>();
                                success = true;
                            } catch (const std::exception& e) {
                                errorMessage = "[AI Error] HTTP or JSON error: " + std::string(e.what());
                                success = false;
                            }
                        }

                        // Now, acquire a single lock to update all logs and state
                        {
                            std::lock_guard<std::mutex> lock(logMutex);
                            logMessages.push_back("OPERATOR: " + message);
                            if (success) {
                                logMessages.push_back("[AI Raw Response] " + ollamaResponse);
                                logMessages.push_back("[AI] " + aiText);

                                json userMsgJson;
                                userMsgJson["role"] = "user";
                                userMsgJson["content"] = message;
                                ollamaMessages.push_back(userMsgJson);

                                json assistantMsgJson;
                                assistantMsgJson["role"] = "assistant";
                                assistantMsgJson["content"] = aiText;
                                ollamaMessages.push_back(assistantMsgJson);

                                while (ollamaMessages.size() > MAX_HISTORY_LENGTH) {
                                    ollamaMessages.erase(ollamaMessages.begin() + 1);
                                }
                            } else if (!errorMessage.empty()) {
                                logMessages.push_back(errorMessage);
                            }
                            scrollToBottom = true;
                        }
                    }
                }
                ImGui::PopItemWidth();
                ImGui::EndDisabled();
            }
            ImGui::End();
        }
    }

    std::string name;
    bool enabled = true;

    // UI State
    bool voiceHuntActive = false;
    bool atakAiActive = false;
    char chatInputBuffer[256] = { 0 };
    bool showLogWindow = false; // New member for pop-out window
    
    // Log State
    std::vector<std::string> logMessages;
    std::mutex logMutex;
    int lastLogSize = 0;
    bool scrollToBottom = false;

    // Audio Processing State
    std::string selectedStreamName = "Radio";
    dsp::stream<dsp::stereo_t>* audioStream = NULL;
    dsp::sink::Handler<float> audioSink;
    dsp::routing::Splitter<dsp::stereo_t> splitter;
    dsp::stream<dsp::stereo_t> splitterOutput; // Output stream for stereo_t
    dsp::convert::StereoToMono stereoToMono; // Stereo to Mono converter
    dsp::stream<float> monoOutput; // Output stream for stereoToMono
    dsp::multirate::RationalResampler<float> resampler; // Resampler for Whisper
    
    // Whisper State
    whisper_context* whisperCtx = nullptr;
    std::vector<float> audioBuffer;
    std::mutex audioBufferMutex;
    std::thread whisperWorker;
    std::atomic<bool> stopWhisperWorker = false;

    // Ollama State
    std::vector<json> ollamaMessages;
    bool ollamaInitialized = false; // Flag for lazy initialization
    const size_t MAX_HISTORY_LENGTH = 10; // Max messages to keep in history (user + assistant)
    std::vector<std::string> availableModels;
    int selectedModelIndex = 0;
    bool modelsLoaded = false;
    std::atomic<bool> ollamaRunning = false;
    std::thread ollamaMonitorThread;
    std::atomic<bool> stopOllamaMonitor = false;
    std::atomic<bool> isWarmingModel = false;
    std::string warmingStatusMessage = "";

private:
    void warmupModel(int newModelIndex, int oldModelIndex);
};

void AtakSigintModule::warmupModel(int newModelIndex, int oldModelIndex) {
    isWarmingModel = true;
    
    // Unload the old model first
    if (oldModelIndex >= 0 && oldModelIndex < availableModels.size()) {
        std::string oldModelName = availableModels[oldModelIndex];
        {
            std::lock_guard<std::mutex> lock(logMutex);
            warmingStatusMessage = "Unloading model: " + oldModelName + "...";
            logMessages.push_back("[OLLAMA] " + warmingStatusMessage);
        }
        try {
            json unloadPayload;
            unloadPayload["model"] = oldModelName;
            unloadPayload["prompt"] = "";
            unloadPayload["keep_alive"] = 0;

            net::http::Client httpClient;
            httpClient.post("http://localhost:11434/api/generate", unloadPayload.dump());
            
            {
                std::lock_guard<std::mutex> lock(logMutex);
                logMessages.push_back("[OLLAMA] Model '" + oldModelName + "' unloaded.");
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(logMutex);
            logMessages.push_back("[OLLAMA Error] Failed to unload model: " + oldModelName + " - " + e.what());
        }
    }

    // Now, warm up the new model
    std::string newModelName;
    if (newModelIndex >= 0 && newModelIndex < availableModels.size()) {
        newModelName = availableModels[newModelIndex];
    } else {
        isWarmingModel = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(logMutex);
        warmingStatusMessage = "Warming model: " + newModelName + "...";
        logMessages.push_back("[OLLAMA] " + warmingStatusMessage);
    }

    try {
        json warmupPayload;
        warmupPayload["model"] = newModelName;
        warmupPayload["messages"] = json::array({
            json::object({{"role", "user"}, {"content", "Hello"}})
        });
        warmupPayload["stream"] = false;

        net::http::Client httpClient;
        httpClient.post("http://localhost:11434/api/chat", warmupPayload.dump());

        {
            std::lock_guard<std::mutex> lock(logMutex);
            warmingStatusMessage = "Model '" + newModelName + "' is ready.";
            logMessages.push_back("[OLLAMA] " + warmingStatusMessage);
        }
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(logMutex);
        warmingStatusMessage = "Failed to warm model: " + newModelName;
        logMessages.push_back("[OLLAMA Error] " + warmingStatusMessage + " - " + e.what());
    }

    std::this_thread::sleep_for(std::chrono::seconds(2));
    warmingStatusMessage = "";
    isWarmingModel = false;
}

MOD_EXPORT void _INIT_() {}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new AtakSigintModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (AtakSigintModule*)instance;
}

MOD_EXPORT void _END_() {}
