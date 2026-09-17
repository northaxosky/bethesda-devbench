#pragma once

#include "GameProfile.h"
#include "ToolRegistry.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace dvb::tools::recording
{
	inline constexpr std::string_view kRecordingFormat = "devbench-recording-3";
	inline constexpr std::size_t      kMaximumRecordingBytes = 16 * 1024 * 1024;
	inline constexpr std::size_t      kMaximumTrajectorySamples = 10000;
	inline constexpr std::size_t      kMaximumActivityEvents = 10000;
	inline constexpr std::size_t      kMaximumCheckpoints = 256;
	inline constexpr std::chrono::milliseconds kMaximumRecordingDuration{
		10 * 60 * 1000
	};

	struct ParsedRecording
	{
		json                     document;
		std::string              format;
		bool                     legacy = false;
		bool                     foreignCapability = false;
		std::vector<std::string> warnings;
	};

	struct RecordingCompatibility
	{
		std::vector<std::string> gameIds;
		std::vector<std::string> runtimeVariants;
		bool                     supportsVR = false;
	};

	RecordingCompatibility RecordingCompatibilityForProfile(
		const GameProfile& a_profile);

	// Parses and validates the shared record/recordings/replay model. Foreign
	// game/VR capabilities remain a hard error unless the caller explicitly
	// accepts the downgrade.
	ParsedRecording ParseRecording(
		json a_document, bool a_allowForeignDowngrade = false);
	ParsedRecording ParseRecording(
		json a_document, const RecordingCompatibility& a_expected,
		bool a_allowForeignDowngrade = false);

	std::string SerializeRecording(const json& a_document);

	class RecordingStore
	{
	public:
		explicit RecordingStore(std::filesystem::path a_root);

		const std::filesystem::path& Root() const noexcept;
		std::filesystem::path       ResolveFile(std::string_view a_file) const;
		json                        Load(std::string_view a_file) const;
		json                        List() const;
		std::string                 WriteUnique(const json& a_document);
		void                        Replace(std::string_view a_file, const json& a_document);
		void                        Delete(std::string_view a_file);

	private:
		void WriteAtomic(
			const std::filesystem::path& a_path, const json& a_document,
			bool a_replace);

		std::filesystem::path root_;
	};

	json SummarizeRecording(
		std::string_view a_file, const ParsedRecording& a_recording);
}
