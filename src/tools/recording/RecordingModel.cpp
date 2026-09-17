#include "tools/recording/RecordingModel.h"

#include "io/WindowsPaths.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

#ifdef _WIN32
#	include <Windows.h>
#endif

namespace dvb::tools::recording
{
	namespace
	{
		namespace fs = std::filesystem;

		std::string PathString(const fs::path& a_path)
		{
			const auto value = a_path.generic_u8string();
			return { reinterpret_cast<const char*>(value.data()), value.size() };
		}

		std::string LowerAscii(std::string a_value)
		{
			std::ranges::transform(a_value, a_value.begin(), [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return a_value;
		}

		void RequireArray(
			const json& a_document, std::string_view a_key, std::size_t a_limit)
		{
			const auto value = a_document.find(a_key);
			if (value == a_document.end())
				return;
			if (!value->is_array())
				throw ToolError(
					400, std::format("'{}' must be an array", a_key));
			if (value->size() > a_limit)
				throw ToolError(
					400,
					std::format(
						"'{}' exceeds the {} entry limit", a_key, a_limit));
		}

		bool ContainsVrClaim(const json& a_value)
		{
			if (a_value.is_string())
			{
				const auto value = LowerAscii(a_value.get<std::string>());
				return value == "vr" || value == "fallout4vr" ||
				       value == "fallout 4 vr" || value.starts_with("vr.") ||
				       value.ends_with(".vr") ||
				       value.find("virtualreality") != std::string::npos;
			}
			if (a_value.is_array())
				return std::ranges::any_of(
					a_value, [](const json& a_item) { return ContainsVrClaim(a_item); });
			if (a_value.is_object())
			{
				for (const auto& [key, value] : a_value.items())
					if (LowerAscii(key) == "vr" ||
						LowerAscii(key).starts_with("vr.") ||
						LowerAscii(key).ends_with(".vr") ||
						ContainsVrClaim(value))
						return true;
			}
			return false;
		}

		bool ContainsAsciiCaseInsensitive(
			const std::vector<std::string>& a_values, std::string_view a_expected)
		{
			const auto expected = LowerAscii(std::string(a_expected));
			return std::ranges::any_of(
				a_values, [&](const std::string& a_value) {
					return LowerAscii(a_value) == expected;
				});
		}

		bool RuntimeCompatible(
			const json& a_runtime, const RecordingCompatibility& a_expected)
		{
			if (!a_runtime.is_object() || a_expected.runtimeVariants.empty())
				return true;
			const auto compatibility = a_runtime.find("compat");
			if (compatibility == a_runtime.end())
				return true;
			if (!compatibility->is_array())
				throw ToolError(400, "'meta.runtime.compat' must be an array");
			bool sawString = false;
			for (const auto& value : *compatibility)
			{
				if (!value.is_string())
					throw ToolError(
						400, "'meta.runtime.compat' entries must be strings");
				sawString = true;
				if (ContainsAsciiCaseInsensitive(
						a_expected.runtimeVariants, value.get<std::string>()))
					return true;
			}
			return !sawString;
		}

		void ValidateCheckpoints(const json& a_meta)
		{
			const auto checkpoints = a_meta.find("checkpoints");
			if (checkpoints == a_meta.end())
				return;
			if (!checkpoints->is_array())
				throw ToolError(400, "'meta.checkpoints' must be an array");
			if (checkpoints->size() > kMaximumCheckpoints)
				throw ToolError(
					400,
					std::format(
						"'meta.checkpoints' exceeds the {} entry limit",
						kMaximumCheckpoints));
			std::set<std::string> ids;
			for (const auto& checkpoint : *checkpoints)
			{
				if (!checkpoint.is_object())
					throw ToolError(400, "each checkpoint must be an object");
				const auto id = checkpoint.find("id");
				if (id == checkpoint.end() || !id->is_string() ||
					id->get_ref<const std::string&>().empty())
					throw ToolError(400, "each checkpoint requires a non-empty string 'id'");
				if (!ids.insert(id->get<std::string>()).second)
					throw ToolError(
						400,
						std::format(
							"duplicate checkpoint id '{}'", id->get<std::string>()));
				if (id->get_ref<const std::string&>().size() > 120 ||
					id->get_ref<const std::string&>().find("..") !=
						std::string::npos ||
					id->get_ref<const std::string&>().find_first_of("/\\:") !=
						std::string::npos)
					throw ToolError(
						400,
						std::format(
							"checkpoint id '{}' is not a portable artifact segment",
							id->get<std::string>()));
				const auto at = checkpoint.find("atMs");
				if (at == checkpoint.end() ||
					(!at->is_number_integer() && !at->is_number_unsigned()) ||
					(!at->is_number_unsigned() && at->get<std::int64_t>() < 0) ||
					(at->is_number_unsigned() &&
						at->get<std::uint64_t>() >
							static_cast<std::uint64_t>(
								kMaximumRecordingDuration.count())) ||
					(at->is_number_integer() &&
						at->get<std::int64_t>() >
							kMaximumRecordingDuration.count()))
					throw ToolError(
						400,
						std::format(
							"checkpoint '{}' requires non-negative integer 'atMs'",
							id->get<std::string>()));
			}
		}

		std::int64_t ReadTimelineTime(
			const json& a_item, std::string_view a_subject)
		{
			const auto at = a_item.find("tMs");
			if (at == a_item.end() ||
				(!at->is_number_integer() && !at->is_number_unsigned()) ||
				(!at->is_number_unsigned() && at->get<std::int64_t>() < 0))
				throw ToolError(
					400,
					std::format(
						"each {} requires non-negative integer 'tMs'",
						a_subject));
			const auto value = at->is_number_unsigned() ?
			                       at->get<std::uint64_t>() :
			                       static_cast<std::uint64_t>(
									   at->get<std::int64_t>());
			if (value >
				static_cast<std::uint64_t>(
					kMaximumRecordingDuration.count()))
				throw ToolError(
					400,
					std::format(
						"{} 'tMs' exceeds the {} ms recording limit",
						a_subject, kMaximumRecordingDuration.count()));
			return static_cast<std::int64_t>(value);
		}

		void ValidateSamples(const json& a_samples)
		{
			std::int64_t previous = 0;
			for (const auto& sample : a_samples)
			{
				if (!sample.is_object())
					throw ToolError(400, "each recording sample must be an object");
				const auto at = ReadTimelineTime(sample, "recording sample");
				if (at < previous)
					throw ToolError(
						400, "recording sample 'tMs' values must be monotonic");
				previous = at;
				const auto pose = sample.find("pose");
				if (pose == sample.end() || !pose->is_array() ||
					pose->size() != 5)
					throw ToolError(
						400,
						"each recording sample requires pose [x,y,z,yaw,pitch]");
				for (const auto& coordinate : *pose)
					if (!coordinate.is_number() ||
						!std::isfinite(coordinate.get<double>()))
						throw ToolError(
							400,
							"recording pose values must be finite numbers");
			}
		}

		void ValidateActivity(const json& a_events)
		{
			for (const auto& event : a_events)
			{
				if (!event.is_object())
					throw ToolError(
						400, "each recording activity event must be an object");
				(void)ReadTimelineTime(event, "recording activity event");
			}
		}

		void ValidateTracking(const json& a_samples)
		{
			std::int64_t previous = -1;
			for (const auto& sample : a_samples)
			{
				if (!sample.is_object())
					throw ToolError(
						400, "each recording tracking sample must be an object");
				const auto at = ReadTimelineTime(
					sample, "recording tracking sample");
				if (at <= previous)
					throw ToolError(
						400,
						"recording tracking sample 'tMs' values must be strictly increasing");
				previous = at;
			}
		}

		void ValidateSteps(const json& a_steps)
		{
			for (const auto& step : a_steps)
				if (!step.is_object())
					throw ToolError(400, "each recording step must be an object");
		}

		bool IsSafeFilename(std::string_view a_file)
		{
			if (a_file.empty() || a_file == "." || a_file == ".." ||
				a_file.find("..") != std::string_view::npos ||
				a_file.find_first_of("/\\:") != std::string_view::npos ||
				a_file.find('\0') != std::string_view::npos)
				return false;
			const fs::path path{ a_file };
			return !path.has_parent_path() && !path.has_root_path() &&
			       LowerAscii(path.extension().string()) == ".json";
		}

		void PublishFile(
			const fs::path& a_temporary, const fs::path& a_destination,
			bool a_replace)
		{
#ifdef _WIN32
			const DWORD flags = MOVEFILE_WRITE_THROUGH |
			                    (a_replace ? MOVEFILE_REPLACE_EXISTING : 0);
			if (!MoveFileExW(
					a_temporary.c_str(), a_destination.c_str(), flags))
				throw ToolError(
					500,
					std::format(
						"could not atomically publish recording '{}' (Win32 error {})",
						PathString(a_destination),
						GetLastError()));
#else
			std::error_code error;
			if (!a_replace && fs::exists(a_destination, error))
				throw ToolError(
					409,
					std::format(
						"recording '{}' already exists",
						a_destination.filename().string()));
			fs::rename(a_temporary, a_destination, error);
			if (error)
				throw ToolError(
					500,
					std::format(
						"could not atomically publish recording '{}': {}",
						PathString(a_destination), error.message()));
#endif
		}
	}

	ParsedRecording ParseRecording(
		json a_document, bool a_allowForeignDowngrade)
	{
		return ParseRecording(
			std::move(a_document),
			RecordingCompatibilityForProfile(Fallout4Profile()),
			a_allowForeignDowngrade);
	}

	RecordingCompatibility RecordingCompatibilityForProfile(
		const GameProfile& a_profile)
	{
		return {
			.gameIds = a_profile.recordingGameIds,
			.runtimeVariants = a_profile.recordingRuntimeCompatibility,
			.supportsVR = a_profile.vr,
		};
	}

	ParsedRecording ParseRecording(
		json a_document, const RecordingCompatibility& a_expected,
		bool a_allowForeignDowngrade)
	{
		if (!a_document.is_object())
			throw ToolError(400, "recording root must be an object");
		auto meta = a_document.find("meta");
		if (meta == a_document.end())
		{
			a_document["meta"] = json::object();
			meta = a_document.find("meta");
		}
		if (!meta->is_object())
			throw ToolError(400, "'meta' must be an object");

		ParsedRecording out;
		const auto format = meta->find("format");
		if (format != meta->end() && !format->is_string())
			throw ToolError(400, "'meta.format' must be a string");
		out.format =
			format == meta->end() ? std::string{} : format->get<std::string>();
		if (out.format.empty())
		{
			out.format = "devbench-recording-1";
			out.legacy = true;
			out.warnings.emplace_back("unmarked legacy recording format");
		}
		else if (
			out.format == "devbench-recording-1" ||
			out.format == "devbench-recording-2")
		{
			out.legacy = true;
			out.warnings.emplace_back(
				std::format("legacy recording format '{}'", out.format));
		}
		else if (out.format != kRecordingFormat)
			throw ToolError(
				400,
				std::format("unsupported recording format '{}'", out.format));

		const auto steps = a_document.find("steps");
		if (steps == a_document.end() || !steps->is_array())
			throw ToolError(400, "recording requires a 'steps' array");
		if (steps->size() > kMaximumTrajectorySamples)
			throw ToolError(
				400,
				std::format(
					"recording steps exceed the {} replay limit",
					kMaximumTrajectorySamples));
		ValidateSteps(*steps);
		RequireArray(a_document, "activityEvents", kMaximumActivityEvents);
		RequireArray(a_document, "samples", kMaximumTrajectorySamples);
		RequireArray(a_document, "trackingSamples", kMaximumTrajectorySamples);
		if (const auto samples = a_document.find("samples");
			samples != a_document.end())
			ValidateSamples(*samples);
		if (const auto activity = a_document.find("activityEvents");
			activity != a_document.end())
			ValidateActivity(*activity);
		if (const auto tracking = a_document.find("trackingSamples");
			tracking != a_document.end())
			ValidateTracking(*tracking);
		ValidateCheckpoints(*meta);

		const auto gameValue = meta->find("game");
		if (gameValue != meta->end() && !gameValue->is_string())
			throw ToolError(400, "'meta.game' must be a string");
		const auto game = LowerAscii(
			gameValue == meta->end() ?
				std::string{} :
				gameValue->get<std::string>());
		const bool explicitForeign =
			!game.empty() &&
			!ContainsAsciiCaseInsensitive(a_expected.gameIds, game);
		const auto runtime = meta->value("runtime", json(nullptr));
		if (!runtime.is_null() && !runtime.is_object())
			throw ToolError(400, "'meta.runtime' must be an object");
		if (runtime.is_object())
		{
			const auto recordedOnVr = runtime.find("recordedOnVR");
			if (recordedOnVr != runtime.end() && !recordedOnVr->is_boolean())
				throw ToolError(
					400, "'meta.runtime.recordedOnVR' must be a boolean");
		}
		const auto tracking = a_document.find("trackingSamples");
		const bool hasTracking =
			tracking != a_document.end() && tracking->is_array() &&
			!tracking->empty();
		const bool vrClaim =
			(runtime.is_object() &&
				runtime.value("recordedOnVR", false)) ||
			hasTracking ||
			ContainsVrClaim(meta->value("capabilities", json::array()));
		const bool unmarkedForeign =
			game.empty() && out.legacy &&
			(vrClaim || meta->contains("trackingCapture")) &&
			!a_expected.supportsVR;
		const bool incompatibleVr = vrClaim && !a_expected.supportsVR;
		const bool incompatibleRuntime = !RuntimeCompatible(runtime, a_expected);
		out.foreignCapability =
			explicitForeign || incompatibleVr || incompatibleRuntime ||
			unmarkedForeign;
		if (out.foreignCapability)
		{
			if (!a_allowForeignDowngrade)
				throw ToolError(
					409,
					"recording declares or implies a foreign game/VR capability; "
					"pass force=true to accept a visible degraded replay");
			out.warnings.emplace_back(
				"foreign game/VR capability explicitly downgraded; unsupported fields are observational only");
		}

		out.document = std::move(a_document);
		return out;
	}

	std::string SerializeRecording(const json& a_document)
	{
		std::string output =
			"{\n\"meta\": " + a_document.value("meta", json::object()).dump(2);
		const auto AppendCompactArray = [&](std::string_view a_key) {
			const auto value = a_document.find(a_key);
			if (value == a_document.end() || !value->is_array())
				return;
			output += ",\n" + json(a_key).dump() + ": [\n";
			for (std::size_t index = 0; index < value->size(); ++index)
				output += (*value)[index].dump() +
				          (index + 1 < value->size() ? ",\n" : "\n");
			output += "]";
		};

		for (const auto& [key, value] : a_document.items())
		{
			if (key == "meta" || key == "steps" || key == "activityEvents" ||
				key == "samples" || key == "trackingSamples")
				continue;
			output += ",\n" + json(key).dump() + ": " + value.dump(2);
		}
		AppendCompactArray("samples");
		AppendCompactArray("trackingSamples");
		AppendCompactArray("activityEvents");
		AppendCompactArray("steps");
		output += "\n}\n";
		if (output.size() > kMaximumRecordingBytes)
			throw ToolError(
				413,
				std::format(
					"serialized recording exceeds the {} byte limit",
					kMaximumRecordingBytes));
		return output;
	}

	RecordingStore::RecordingStore(fs::path a_root) :
		root_(fs::absolute(std::move(a_root)).lexically_normal())
	{}

	const fs::path& RecordingStore::Root() const noexcept
	{
		return root_;
	}

	fs::path RecordingStore::ResolveFile(std::string_view a_file) const
	{
		if (!IsSafeFilename(a_file))
			throw ToolError(
				400,
				"'file' must be a bare .json recording name without traversal");
		return root_ / fs::path(a_file);
	}

	json RecordingStore::Load(std::string_view a_file) const
	{
		const auto path = ResolveFile(a_file);
		std::error_code error;
		if (!fs::exists(path, error) || error)
			throw ToolError(
				404,
				std::format("recording '{}' was not found", a_file));
		if (fs::is_symlink(fs::symlink_status(path, error)) || error)
			throw ToolError(
				400,
				std::format(
					"recording '{}' must not be a symbolic link", a_file));
		const auto bytes = fs::file_size(path, error);
		if (error)
			throw ToolError(
				500,
				std::format(
					"could not inspect recording '{}': {}", a_file, error.message()));
		if (bytes > kMaximumRecordingBytes)
			throw ToolError(
				413,
				std::format(
					"recording '{}' exceeds the {} byte limit",
					a_file, kMaximumRecordingBytes));
		std::ifstream input(path, std::ios::binary);
		if (!input)
			throw ToolError(
				404,
				std::format("recording '{}' could not be opened", a_file));
		json document;
		try
		{
			input >> document;
		}
		catch (const std::exception& error)
		{
			throw ToolError(
				400,
				std::format(
					"recording '{}' contains invalid JSON: {}", a_file, error.what()));
		}
		return document;
	}

	json RecordingStore::List() const
	{
		json entries = json::array();
		std::error_code error;
		if (!fs::exists(root_, error))
			return json{
				{ "dir", PathString(root_) },
				{ "count", 0 },
				{ "recordings", std::move(entries) },
			};
		fs::directory_iterator iterator(root_, error);
		if (error)
			throw ToolError(
				500,
				std::format(
					"could not enumerate recordings root '{}': {}",
					PathString(root_), error.message()));
		for (const fs::directory_iterator end;
			iterator != end;
			iterator.increment(error))
		{
			if (error)
				throw ToolError(
					500,
					std::format(
						"could not continue enumerating recordings: {}",
						error.message()));
			const auto status = iterator->symlink_status(error);
			if (error || fs::is_symlink(status) ||
				!fs::is_regular_file(status) ||
				LowerAscii(iterator->path().extension().string()) != ".json")
				continue;
			const auto file = iterator->path().filename().string();
			try
			{
				entries.push_back(
					SummarizeRecording(file, ParseRecording(Load(file), false)));
			}
			catch (const ToolError& parseError)
			{
				entries.push_back(json{
					{ "file", file },
					{ "error", parseError.what() },
				});
			}
			catch (const std::exception& parseError)
			{
				entries.push_back(json{
					{ "file", file },
					{ "error",
						std::format(
							"malformed recording metadata: {}",
							parseError.what()) },
				});
			}
		}
		std::sort(entries.begin(), entries.end(), [](const json& a_left, const json& a_right) {
			const auto left = a_left.value("recordedAt", std::int64_t{ 0 });
			const auto right = a_right.value("recordedAt", std::int64_t{ 0 });
			if (left != right)
				return left > right;
			return a_left.value("file", std::string{}) <
			       a_right.value("file", std::string{});
		});
		return json{
			{ "dir", PathString(io::PhysicalFilePath(root_)) },
			{ "count", entries.size() },
			{ "recordings", std::move(entries) },
		};
	}

	std::string RecordingStore::WriteUnique(const json& a_document)
	{
		static std::atomic_uint64_t sequence{ 0 };
		const auto stamp =
			std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::system_clock::now().time_since_epoch())
				.count();
		for (std::size_t attempt = 0; attempt < 1000; ++attempt)
		{
			const auto file = std::format(
				"recording_{}_{}.json", stamp, ++sequence);
			const auto path = ResolveFile(file);
			std::error_code error;
			if (fs::exists(path, error))
				continue;
			if (error)
				throw ToolError(
					500,
					std::format(
						"could not inspect recording destination '{}': {}",
						PathString(path), error.message()));
			WriteAtomic(path, a_document, false);
			return file;
		}
		throw ToolError(500, "could not allocate a unique recording filename");
	}

	void RecordingStore::Replace(
		std::string_view a_file, const json& a_document)
	{
		const auto path = ResolveFile(a_file);
		std::error_code error;
		if (!fs::exists(path, error) || error)
			throw ToolError(
				404,
				std::format("recording '{}' was not found", a_file));
		if (fs::is_symlink(fs::symlink_status(path, error)) || error)
			throw ToolError(
				400,
				std::format(
					"recording '{}' must not be a symbolic link", a_file));
		if (!fs::is_regular_file(path, error) || error)
			throw ToolError(
				404,
				std::format("recording '{}' was not found", a_file));
		WriteAtomic(path, a_document, true);
	}

	void RecordingStore::Delete(std::string_view a_file)
	{
		const auto path = ResolveFile(a_file);
		std::error_code error;
		if (!fs::exists(path, error) || error)
			throw ToolError(
				404,
				std::format("recording '{}' was not found", a_file));
		if (fs::is_symlink(fs::symlink_status(path, error)) || error)
			throw ToolError(
				400,
				std::format(
					"recording '{}' must not be a symbolic link", a_file));
		if (!fs::is_regular_file(path, error) || error)
			throw ToolError(
				400,
				std::format("recording '{}' is not a regular file", a_file));
		if (!fs::remove(path, error) || error)
			throw ToolError(
				500,
				std::format(
					"could not delete recording '{}': {}",
					a_file, error.message()));
	}

	void RecordingStore::WriteAtomic(
		const fs::path& a_path, const json& a_document, bool a_replace)
	{
		const auto serialized = SerializeRecording(a_document);
		std::error_code error;
		fs::create_directories(root_, error);
		if (error)
			throw ToolError(
				500,
				std::format(
					"could not create recordings root '{}': {}",
					PathString(root_), error.message()));
		if (!a_replace && fs::exists(a_path, error))
			throw ToolError(
				409,
				std::format(
					"recording '{}' already exists",
					a_path.filename().string()));

		static std::atomic_uint64_t temporarySequence{ 0 };
		const auto temporary = root_ / std::format(
			".{}.tmp.{}",
			a_path.filename().string(),
			++temporarySequence);
		try
		{
			std::ofstream output(
				temporary, std::ios::binary | std::ios::trunc);
			if (!output)
				throw ToolError(
					500,
					std::format(
						"could not create temporary recording '{}'",
						PathString(temporary)));
			output.write(
				serialized.data(),
				static_cast<std::streamsize>(serialized.size()));
			output.flush();
			if (!output)
				throw ToolError(
					500,
					std::format(
						"could not flush temporary recording '{}'",
						PathString(temporary)));
			output.close();
			PublishFile(temporary, a_path, a_replace);
			const auto bytes = fs::file_size(a_path, error);
			if (error || bytes != serialized.size())
				throw ToolError(
					500,
					std::format(
						"recording '{}' failed post-publication verification",
						a_path.filename().string()));
		}
		catch (...)
		{
			std::error_code ignored;
			fs::remove(temporary, ignored);
			throw;
		}
	}

	json SummarizeRecording(
		std::string_view a_file, const ParsedRecording& a_recording)
	{
		const auto meta = a_recording.document.value("meta", json::object());
		json       out{
			{ "file", a_file },
			{ "format", a_recording.format },
			{ "legacy", a_recording.legacy },
			{ "foreignCapability", a_recording.foreignCapability },
			{ "runtime",
				meta.value("runtime", json::object()).value(
					"compat", json::array()) },
			{ "entry", meta.value("entryPoint", json::object()) },
		};
		for (const auto key : { "name", "game", "cell", "worldspace", "interior",
				 "sampleCount", "recordedMs", "recordedAt", "validated" })
			if (meta.contains(key))
				out[key] = meta[key];
		if (!a_recording.warnings.empty())
			out["warnings"] = a_recording.warnings;
		return out;
	}
}
