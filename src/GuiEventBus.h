#pragma once

#include <string>
#include <functional>
#include <vector>

/**
 * GuiEventBus - Simple event bus for GUI-related events
 *
 * Used to communicate loading progress, errors, and other UI events
 * across different parts of the engine without tight coupling.
 */
class GuiEventBus {
public:
	// Singleton access
	static GuiEventBus& GetInstance() {
		static GuiEventBus instance;
		return instance;
	}

	// Event callbacks
	using ProgressCallback = std::function<void(float progress, const std::string& stage)>;
	using ErrorCallback = std::function<void(const std::string& error)>;
	using LoadingStartedCallback = std::function<void()>;
	using LoadingCompletedCallback = std::function<void()>;

	// Subscribe to events
	void SubscribeProgress(ProgressCallback callback) {
		m_progressCallbacks.push_back(callback);
	}

	void SubscribeError(ErrorCallback callback) {
		m_errorCallbacks.push_back(callback);
	}

	void SubscribeLoadingStarted(LoadingStartedCallback callback) {
		m_loadingStartedCallbacks.push_back(callback);
	}

	void SubscribeLoadingCompleted(LoadingCompletedCallback callback) {
		m_loadingCompletedCallbacks.push_back(callback);
	}

	// Publish events
	void PublishProgress(float progress, const std::string& stage) {
		for (auto& callback : m_progressCallbacks) {
			if (callback) callback(progress, stage);
		}
	}

	void PublishError(const std::string& error) {
		for (auto& callback : m_errorCallbacks) {
			if (callback) callback(error);
		}
	}

	void PublishLoadingStarted() {
		for (auto& callback : m_loadingStartedCallbacks) {
			if (callback) callback();
		}
	}

	void PublishLoadingCompleted() {
		for (auto& callback : m_loadingCompletedCallbacks) {
			if (callback) callback();
		}
	}

	// Clear all callbacks (useful when changing scenes)
	void ClearCallbacks() {
		m_progressCallbacks.clear();
		m_errorCallbacks.clear();
		m_loadingStartedCallbacks.clear();
		m_loadingCompletedCallbacks.clear();
	}

private:
	GuiEventBus() = default;
	~GuiEventBus() = default;
	GuiEventBus(const GuiEventBus&) = delete;
	GuiEventBus& operator=(const GuiEventBus&) = delete;

	std::vector<ProgressCallback> m_progressCallbacks;
	std::vector<ErrorCallback> m_errorCallbacks;
	std::vector<LoadingStartedCallback> m_loadingStartedCallbacks;
	std::vector<LoadingCompletedCallback> m_loadingCompletedCallbacks;
};
