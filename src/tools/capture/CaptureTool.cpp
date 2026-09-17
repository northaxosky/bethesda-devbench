#include "CaptureTool.h"

#include "Config.h"
#include "Ssim.h"
#include "ToolExtensions.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <set>
#include <system_error>
#include <thread>

namespace dvb::tools::capture
{
	namespace
	{
		namespace fs = std::filesystem;
		using namespace std::chrono;

		constexpr std::string_view kImageExtensions[] = {
			".bmp", ".png", ".jpg", ".jpeg", ".dds"
		};

		struct ScannedFile
		{
			fs::path           path;
			std::uintmax_t     size = 0;
			fs::file_time_type modified{};
		};

		struct ArtifactObservation
		{
			std::uintmax_t     size = 0;
			fs::file_time_type modified{};
			int                stableSamples = 0;
		};

		struct ArtifactProbe
		{
			bool           complete = false;
			bool           invalid = false;
			std::string    error;
			std::uintmax_t bytes = 0;
			int            width = 0;
			int            height = 0;
		};

		struct AwaitedArtifact
		{
			std::uintmax_t      bytes = 0;
			int                 width = 0;
			int                 height = 0;
			std::string         readyBy;
			std::optional<bool> uiExcluded;
			std::optional<bool> subrectApplied;
			json                providerDegraded = json::array();
			std::optional<json> eventGap;
		};

		std::string JsonPath(const fs::path& a_path)
		{
			const auto bytes = a_path.generic_u8string();
			return { reinterpret_cast<const char*>(bytes.data()), bytes.size() };
		}

		std::string LowerAscii(std::string a_value)
		{
			std::ranges::transform(a_value, a_value.begin(), [](unsigned char a_ch) {
				return static_cast<char>(
					a_ch >= 'A' && a_ch <= 'Z' ? a_ch + ('a' - 'A') : a_ch);
			});
			return a_value;
		}

		std::string PathKey(const fs::path& a_path)
		{
			std::error_code ec;
			auto            absolute = fs::absolute(a_path, ec);
			if (ec)
				absolute = a_path;
			return LowerAscii(JsonPath(absolute.lexically_normal()));
		}

		void RequireObject(const json& a_args, std::string_view a_operation)
		{
			if (!a_args.is_object())
				throw ToolError(400, std::format("{} arguments must be an object", a_operation));
		}

		std::string ReadString(
			const json& a_args, std::string_view a_name, std::string a_default = {})
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_string())
				throw ToolError(400, std::format("'{}' must be a string", a_name));
			auto value = it->get<std::string>();
			if (value.find('\0') != std::string::npos)
				throw ToolError(400, std::format("'{}' must not contain an embedded NUL", a_name));
			return value;
		}

		bool ReadBool(const json& a_args, std::string_view a_name, bool a_default)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return a_default;
			if (!it->is_boolean())
				throw ToolError(400, std::format("'{}' must be a boolean", a_name));
			return it->get<bool>();
		}

		long ReadMilliseconds(
			const json& a_args, std::string_view a_name, milliseconds a_default, long a_min, long a_max)
		{
			const auto it = a_args.find(a_name);
			if (it == a_args.end())
				return static_cast<long>(a_default.count());
			if (!it->is_number_integer())
				throw ToolError(400, std::format("'{}' must be an integer", a_name));
			const auto value = it->get<long long>();
			if (value < a_min || value > a_max)
				throw ToolError(
					400, std::format("'{}' must be between {} and {} milliseconds", a_name, a_min, a_max));
			return static_cast<long>(value);
		}

		void ValidatePathSegment(std::string_view a_name, std::string_view a_value)
		{
			if (a_value.empty() || a_value.size() > 120 || a_value == "." || a_value == "..")
				throw ToolError(400, std::format(
										 "'{}' must be a non-empty path segment of at most 120 bytes",
										 a_name));
			if (a_value.ends_with('.') || a_value.ends_with(' '))
				throw ToolError(400, std::format("'{}' must not end with a dot or space", a_name));
			for (const unsigned char ch : a_value)
			{
				if (ch < 0x20 || ch >= 0x7F || std::string_view("<>:\"/\\|?*").contains(static_cast<char>(ch)))
					throw ToolError(400, std::format(
											 "'{}' must be one portable ASCII path segment with no traversal or separators",
											 a_name));
			}
			const auto lower = LowerAscii(std::string(a_value.substr(0, a_value.find('.'))));
			const bool reserved =
				lower == "con" || lower == "prn" || lower == "aux" || lower == "nul" ||
				(lower.size() == 4 &&
					((lower.starts_with("com") || lower.starts_with("lpt")) &&
						lower[3] >= '1' && lower[3] <= '9'));
			if (reserved)
				throw ToolError(400, std::format("'{}' is a reserved Windows filename", a_name));
		}

		void ValidateRelativeRoot(const fs::path& a_path)
		{
			if (a_path.empty())
				throw ToolError(400, "'outDir' must not be empty");
			if (a_path.is_absolute())
				return;
			for (const auto& segment : a_path)
			{
				if (segment == "..")
					throw ToolError(400, "'outDir' must not traverse above the game root");
			}
		}

		void ValidateUvRect(const json& a_rect, std::string_view a_name)
		{
			if (!a_rect.is_object())
				throw ToolError(400, std::format("'{}' must be an object", a_name));
			for (const auto key : { "x", "y", "w", "h" })
			{
				const auto it = a_rect.find(key);
				if (it == a_rect.end() || !it->is_number())
					throw ToolError(400, std::format("'{}' requires numeric x, y, w, and h", a_name));
				const auto value = it->get<double>();
				if (!std::isfinite(value))
					throw ToolError(400, std::format("'{}' values must be finite", a_name));
			}
			const double x = a_rect.at("x").get<double>();
			const double y = a_rect.at("y").get<double>();
			const double w = a_rect.at("w").get<double>();
			const double h = a_rect.at("h").get<double>();
			if (x < 0.0 || y < 0.0 || w <= 0.0 || h <= 0.0 || x + w > 1.0 || y + h > 1.0)
				throw ToolError(400, std::format(
										 "'{}' must be a non-empty UV rectangle contained within 0..1",
										 a_name));
		}

		void ValidateComparison(const json& a_args)
		{
			if (const auto it = a_args.find("golden"); it != a_args.end())
			{
				if (!it->is_string() || it->get_ref<const std::string&>().empty())
					throw ToolError(400, "'golden' must be a non-empty path string");
			}
			if (const auto it = a_args.find("threshold"); it != a_args.end())
			{
				if (!it->is_number())
					throw ToolError(400, "'threshold' must be numeric");
				const auto value = it->get<double>();
				if (!std::isfinite(value) || value < -1.0 || value > 1.0)
					throw ToolError(400, "'threshold' must be finite and between -1 and 1");
			}
			if (const auto it = a_args.find("regions"); it != a_args.end())
			{
				if (!it->is_array() || it->size() > 64)
					throw ToolError(400, "'regions' must be an array with at most 64 entries");
				for (const auto& region : *it)
				{
					ValidateUvRect(region, "regions[]");
					if (const auto name = region.find("name");
						name != region.end() && (!name->is_string() || name->get_ref<const std::string&>().size() > 120))
						throw ToolError(400, "region 'name' must be a string of at most 120 bytes");
					if (const auto threshold = region.find("threshold"); threshold != region.end())
					{
						if (!threshold->is_number())
							throw ToolError(400, "region 'threshold' must be numeric");
						const auto value = threshold->get<double>();
						if (!std::isfinite(value) || value < -1.0 || value > 1.0)
							throw ToolError(400, "region 'threshold' must be finite and between -1 and 1");
					}
				}
			}
		}

		void ValidateCaptureRequest(const json& a_args)
		{
			RequireObject(a_args, "capture");
			const auto checkpoint = ReadString(a_args, "checkpointId");
			if (checkpoint.empty())
				throw ToolError(400, "capture requires 'checkpointId'");
			ValidatePathSegment("checkpointId", checkpoint);
			ValidatePathSegment("recording", ReadString(a_args, "recording", "adhoc"));
			ValidatePathSegment("variant", ReadString(a_args, "variant", "default"));
			if (const auto it = a_args.find("repeat"); it != a_args.end())
			{
				if (!it->is_number_integer() || it->get<long long>() < 0)
					throw ToolError(400, "'repeat' must be a non-negative integer");
			}
			if (const auto it = a_args.find("subrect"); it != a_args.end())
				ValidateUvRect(*it, "subrect");
			if (const auto out = a_args.find("outDir"); out != a_args.end())
			{
				if (!out->is_string())
					throw ToolError(400, "'outDir' must be a string");
				ValidateRelativeRoot(fs::path(out->get<std::string>()));
			}
			(void)ReadBool(a_args, "allowNative", false);
			(void)ReadBool(a_args, "excludeUi", true);
			(void)ReadBool(a_args, "cleanup", false);
			ValidateComparison(a_args);
		}

		std::string CaptureStem(const json& a_args)
		{
			auto stem = a_args.at("checkpointId").get<std::string>();
			if (const auto repeat = a_args.find("repeat"); repeat != a_args.end())
				stem += "__r" + std::to_string(repeat->get<long long>());
			ValidatePathSegment("capture stem", stem);
			return stem;
		}

		fs::path ResolveRoot(const CaptureConfiguration& a_configuration,
			const CaptureBackend& a_backend, const json& a_args)
		{
			fs::path root = a_configuration.captureDirectory;
			if (const auto it = a_args.find("outDir"); it != a_args.end())
				root = fs::path(it->get<std::string>());
			if (root.is_relative())
				root = a_backend.gameRoot() / root;
			return fs::absolute(root).lexically_normal();
		}

		fs::path CaptureDirectory(const CaptureConfiguration& a_configuration,
			const CaptureBackend& a_backend, const json& a_args)
		{
			return ResolveRoot(a_configuration, a_backend, a_args) /
			       a_args.value("recording", std::string("adhoc")) /
			       a_args.value("variant", std::string("default"));
		}

		bool ImageExtension(const fs::path& a_path)
		{
			const auto extension = LowerAscii(a_path.extension().string());
			return std::ranges::find(kImageExtensions, extension) != std::end(kImageExtensions);
		}

		std::vector<ScannedFile> ScanDirectories(
			const std::vector<fs::path>& a_directories, bool a_requireDirectory)
		{
			std::vector<ScannedFile> out;
			for (const auto& directory : a_directories)
			{
				std::error_code ec;
				const bool      exists = fs::exists(directory, ec);
				if (ec)
					throw ToolError(500, std::format(
											 "failed to inspect screenshot directory '{}': {}",
											 JsonPath(directory), ec.message()));
				if (!exists)
				{
					if (a_requireDirectory)
						throw ToolError(404, std::format(
												 "screenshot directory '{}' does not exist",
												 JsonPath(directory)));
					continue;
				}
				if (!fs::is_directory(directory, ec) || ec)
					throw ToolError(400, std::format(
											 "screenshot path '{}' is not a readable directory",
											 JsonPath(directory)));

				fs::directory_iterator iterator(directory, ec);
				if (ec)
					throw ToolError(500, std::format(
											 "failed to enumerate screenshot directory '{}': {}",
											 JsonPath(directory), ec.message()));
				for (const fs::directory_iterator end; iterator != end; iterator.increment(ec))
				{
					if (ec)
						throw ToolError(500, std::format(
												 "failed while enumerating screenshot directory '{}': {}",
												 JsonPath(directory), ec.message()));
					const auto& entry = *iterator;
					if (!entry.is_regular_file(ec))
					{
						if (ec)
							throw ToolError(500, std::format(
													 "failed to inspect screenshot entry '{}': {}",
													 JsonPath(entry.path()), ec.message()));
						continue;
					}
					if (!ImageExtension(entry.path()))
						continue;
					const auto size = entry.file_size(ec);
					if (ec)
						throw ToolError(500, std::format(
												 "failed to read screenshot size '{}': {}",
												 JsonPath(entry.path()), ec.message()));
					const auto modified = entry.last_write_time(ec);
					if (ec)
						throw ToolError(500, std::format(
												 "failed to read screenshot time '{}': {}",
												 JsonPath(entry.path()), ec.message()));
					out.push_back({ entry.path(), size, modified });
					if (out.size() > kMaxScannedImages)
						throw ToolError(413, std::format(
												 "screenshot scan exceeded the {} file budget",
												 kMaxScannedImages));
				}
				if (ec)
					throw ToolError(500, std::format(
											 "failed while enumerating screenshot directory '{}': {}",
											 JsonPath(directory), ec.message()));
			}
			return out;
		}

		bool ExclusiveReadable(const fs::path& a_path)
		{
			const HANDLE file = ::CreateFileW(
				a_path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
				FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
			if (file == INVALID_HANDLE_VALUE)
				return false;
			::CloseHandle(file);
			return true;
		}

		ArtifactProbe ProbeArtifact(const fs::path& a_path, ArtifactObservation& a_observation)
		{
			ArtifactProbe   result;
			std::error_code ec;
			if (!fs::exists(a_path, ec))
			{
				if (ec)
					result.error = std::format("failed to inspect capture '{}': {}", JsonPath(a_path), ec.message());
				return result;
			}
			const auto size = fs::file_size(a_path, ec);
			if (ec)
			{
				result.error = std::format("failed to read capture size '{}': {}", JsonPath(a_path), ec.message());
				return result;
			}
			if (size == 0)
				return result;
			if (size > kMaxCaptureBytes)
			{
				result.invalid = true;
				result.error = std::format(
					"capture '{}' exceeds the {} byte budget", JsonPath(a_path), kMaxCaptureBytes);
				return result;
			}
			const auto modified = fs::last_write_time(a_path, ec);
			if (ec)
			{
				result.error = std::format("failed to read capture time '{}': {}", JsonPath(a_path), ec.message());
				return result;
			}
			if (!ExclusiveReadable(a_path))
				return result;

			if (a_observation.size == size && a_observation.modified == modified)
				++a_observation.stableSamples;
			else
			{
				a_observation.size = size;
				a_observation.modified = modified;
				a_observation.stableSamples = 0;
			}
			if (a_observation.stableSamples < 1)
				return result;

			const auto image = Ssim::DecodeGray(a_path);
			if (!image || image->width <= 0 || image->height <= 0)
			{
				result.invalid = true;
				result.error = std::format(
					"capture '{}' is not a complete decodable PNG or BMP image",
					JsonPath(a_path));
				return result;
			}
			result.complete = true;
			result.bytes = size;
			result.width = image->width;
			result.height = image->height;
			return result;
		}

		void AppendDegraded(json& a_degraded, std::string a_reason)
		{
			if (a_reason.empty())
				return;
			for (const auto& existing : a_degraded)
				if (existing.is_string() && existing.get_ref<const std::string&>() == a_reason)
					return;
			a_degraded.push_back(std::move(a_reason));
		}

		void AppendProviderDegraded(json& a_degraded, const json& a_source)
		{
			const auto it = a_source.find("degraded");
			if (it == a_source.end() || it->is_null() || (it->is_boolean() && !it->get<bool>()))
				return;
			if (it->is_boolean())
			{
				AppendDegraded(a_degraded, "providerDegraded");
				return;
			}
			if (it->is_string())
			{
				AppendDegraded(a_degraded, it->get<std::string>());
				return;
			}
			if (!it->is_array())
				throw ToolError(502, "capture provider 'degraded' must be boolean, string, or string array");
			for (const auto& reason : *it)
			{
				if (!reason.is_string())
					throw ToolError(502, "capture provider 'degraded' array must contain strings");
				AppendDegraded(a_degraded, reason.get<std::string>());
			}
		}

		bool SameOutputPath(const fs::path& a_expected, std::string_view a_reported)
		{
			return PathKey(a_expected) == PathKey(fs::path(a_reported));
		}

		void ObserveReadyEvent(const EventBus::Event& a_event, std::string_view a_requestId,
			const fs::path& a_outputPath, std::optional<json>& a_readyPayload)
		{
			if (a_event.topic != "capture.ready")
				return;
			if (!a_event.payload.is_object())
				throw ToolError(502, "capture provider published a non-object capture.ready payload");
			const auto request = a_event.payload.find("requestId");
			if (request == a_event.payload.end() || !request->is_string())
				throw ToolError(502, "capture provider 'requestId' must be a string");
			if (request->get_ref<const std::string&>() != a_requestId)
				return;
			const auto ok = a_event.payload.find("ok");
			if (ok == a_event.payload.end() || !ok->is_boolean())
				throw ToolError(502, "capture provider 'ok' must be boolean");
			if (!ok->get<bool>())
			{
				if (const auto error = a_event.payload.find("error");
					error != a_event.payload.end() && !error->is_string())
					throw ToolError(502, "capture provider 'error' must be a string");
				throw ToolError(502, a_event.payload.value(
										 "error", std::string("capture provider reported failure")));
			}
			if (const auto path = a_event.payload.find("path"); path != a_event.payload.end())
			{
				if (!path->is_string() || !SameOutputPath(a_outputPath, path->get_ref<const std::string&>()))
					throw ToolError(502, "capture provider reported a path different from its requested outputPath");
			}
			a_readyPayload = a_event.payload;
		}

		AwaitedArtifact AwaitProviderArtifact(EventBus& a_events, std::uint64_t a_startCursor,
			std::string_view a_requestId, const fs::path& a_outputPath,
			milliseconds a_timeout, milliseconds a_poll)
		{
			const auto          deadline = steady_clock::now() + a_timeout;
			std::uint64_t       cursor = a_startCursor;
			std::optional<json> readyPayload;
			std::optional<json> eventGap;
			ArtifactObservation observation;
			std::string         invalidError;
			std::string         ioError;
			auto                WithGap = [&](std::string a_message) {
				if (eventGap)
					a_message += std::format(
						"; EventBus ring gap: expected sequence {}, first available {}",
						eventGap->at("expectedSeq").get<std::uint64_t>(),
						eventGap->at("firstAvailableSeq").get<std::uint64_t>());
				return a_message;
			};

			while (steady_clock::now() < deadline)
			{
				const auto events = a_events.Since(cursor);
				if (!events.empty() && events.front().seq > cursor + 1 && !eventGap)
				{
					eventGap = json{
						{ "expectedSeq", cursor + 1 },
						{ "firstAvailableSeq", events.front().seq },
					};
				}
				for (const auto& event : events)
				{
					cursor = event.seq;
					try
					{
						ObserveReadyEvent(event, a_requestId, a_outputPath, readyPayload);
					}
					catch (const ToolError& error)
					{
						throw ToolError(error.code, WithGap(error.what()));
					}
				}

				const auto probe = ProbeArtifact(a_outputPath, observation);
				if (probe.complete)
				{
					AwaitedArtifact result{
						.bytes = probe.bytes,
						.width = probe.width,
						.height = probe.height,
						.readyBy = readyPayload ? "event" : "poll",
						.eventGap = std::move(eventGap),
					};
					if (readyPayload)
					{
						if (const auto it = readyPayload->find("uiExcluded"); it != readyPayload->end())
						{
							if (!it->is_boolean())
								throw ToolError(502, "capture provider 'uiExcluded' must be boolean");
							result.uiExcluded = it->get<bool>();
						}
						if (const auto it = readyPayload->find("subrectApplied"); it != readyPayload->end())
						{
							if (!it->is_boolean())
								throw ToolError(502, "capture provider 'subrectApplied' must be boolean");
							result.subrectApplied = it->get<bool>();
						}
						AppendProviderDegraded(result.providerDegraded, *readyPayload);
					}
					return result;
				}
				if (probe.invalid)
					invalidError = probe.error;
				else if (!probe.error.empty())
					ioError = probe.error;
				std::this_thread::sleep_for(a_poll);
			}
			if (!ioError.empty())
				throw ToolError(500, WithGap(ioError));
			if (!invalidError.empty())
				throw ToolError(422, WithGap(invalidError));
			throw ToolError(504, WithGap(std::format(
									 "timed out after {}ms waiting for provider request '{}' to produce a complete image",
									 a_timeout.count(), a_requestId)));
		}

		std::vector<fs::path> ScanRoots(
			const CaptureConfiguration& a_configuration, const CaptureBackend& a_backend)
		{
			std::vector<fs::path> directories;
			const auto            root = a_backend.gameRoot();
			directories.reserve(a_configuration.scanDirectories.size());
			for (const auto& configured : a_configuration.scanDirectories)
				directories.push_back(configured.empty() ? root :
														   (configured.is_absolute() ? configured : root / configured));
			return directories;
		}

		std::map<std::string, ScannedFile> IndexFiles(const std::vector<ScannedFile>& a_files)
		{
			std::map<std::string, ScannedFile> indexed;
			for (const auto& file : a_files)
				indexed.insert_or_assign(PathKey(file.path), file);
			return indexed;
		}

		bool ChangedSince(const ScannedFile& a_file, const std::map<std::string, ScannedFile>& a_before)
		{
			const auto existing = a_before.find(PathKey(a_file.path));
			return existing == a_before.end() ||
			       existing->second.size != a_file.size ||
			       existing->second.modified != a_file.modified;
		}

		ScannedFile AwaitNativeArtifact(const CaptureConfiguration& a_configuration,
			const CaptureBackend& a_backend, const std::map<std::string, ScannedFile>& a_before,
			milliseconds a_timeout, milliseconds a_poll)
		{
			const auto                                 deadline = steady_clock::now() + a_timeout;
			const auto                                 roots = ScanRoots(a_configuration, a_backend);
			std::map<std::string, ArtifactObservation> observations;
			std::string                                invalidError;
			while (steady_clock::now() < deadline)
			{
				const auto               files = ScanDirectories(roots, false);
				std::vector<ScannedFile> candidates;
				for (const auto& file : files)
					if (ChangedSince(file, a_before))
						candidates.push_back(file);
				if (candidates.size() > 1)
					throw ToolError(409, std::format(
											 "native screenshot correlation is ambiguous: {} files changed after queueing",
											 candidates.size()));
				if (candidates.size() == 1)
				{
					auto&      observation = observations[PathKey(candidates.front().path)];
					const auto probe = ProbeArtifact(candidates.front().path, observation);
					if (probe.complete)
						return candidates.front();
					if (probe.invalid)
						invalidError = probe.error;
					else if (!probe.error.empty())
						throw ToolError(500, probe.error);
				}
				std::this_thread::sleep_for(a_poll);
			}
			if (!invalidError.empty())
				throw ToolError(422, invalidError);
			std::string scanned;
			for (const auto& root : roots)
				scanned += (scanned.empty() ? "" : ", ") + JsonPath(root);
			throw ToolError(504, std::format(
									 "no unambiguous complete native screenshot appeared within {}ms (scanned: {})",
									 a_timeout.count(), scanned));
		}

		void EnsureNewArtifactPaths(const fs::path& a_image)
		{
			std::error_code ec;
			if (fs::exists(a_image, ec))
				throw ToolError(409, std::format(
										 "capture output '{}' already exists; existing artifacts are never overwritten",
										 JsonPath(a_image)));
			if (ec)
				throw ToolError(500, std::format(
										 "failed to inspect capture output '{}': {}",
										 JsonPath(a_image), ec.message()));
			auto sidecar = a_image;
			sidecar.replace_extension(".json");
			if (fs::exists(sidecar, ec))
				throw ToolError(409, std::format(
										 "capture sidecar '{}' already exists; existing artifacts are never overwritten",
										 JsonPath(sidecar)));
			if (ec)
				throw ToolError(500, std::format(
										 "failed to inspect capture sidecar '{}': {}",
										 JsonPath(sidecar), ec.message()));
		}

		void CreateDirectory(const fs::path& a_directory)
		{
			std::error_code ec;
			fs::create_directories(a_directory, ec);
			if (ec)
				throw ToolError(500, std::format(
										 "failed to create capture directory '{}': {}",
										 JsonPath(a_directory), ec.message()));
		}

		void WriteSidecar(const fs::path& a_imagePath, const json& a_result, std::uint64_t a_sequence)
		{
			const auto body = a_result.dump(2) + '\n';
			if (body.size() > kMaxSidecarBytes)
				throw ToolError(413, std::format(
										 "capture metadata exceeds the {} byte sidecar budget",
										 kMaxSidecarBytes));
			fs::path sidecar = a_imagePath;
			sidecar.replace_extension(".json");
			std::error_code ec;
			if (fs::exists(sidecar, ec))
				throw ToolError(409, std::format(
										 "capture sidecar '{}' already exists; existing metadata is never overwritten",
										 JsonPath(sidecar)));
			if (ec)
				throw ToolError(500, std::format(
										 "failed to inspect capture sidecar '{}': {}",
										 JsonPath(sidecar), ec.message()));
			CreateDirectory(sidecar.parent_path());
			const auto temporary = fs::path(
				sidecar.wstring() + L".dvb-" + std::to_wstring(a_sequence) + L".tmp");
			struct RemoveTemporary
			{
				fs::path path;
				~RemoveTemporary()
				{
					std::error_code ignored;
					fs::remove(path, ignored);
				}
			} remove{ temporary };
			try
			{
				std::ofstream output;
				output.exceptions(std::ios::failbit | std::ios::badbit);
				output.open(temporary, std::ios::binary | std::ios::trunc);
				output.write(body.data(), static_cast<std::streamsize>(body.size()));
				output.close();
			}
			catch (const std::exception& error)
			{
				throw ToolError(500, std::format(
										 "failed to write capture sidecar '{}': {}",
										 JsonPath(sidecar), error.what()));
			}
			if (!::MoveFileExW(temporary.c_str(), sidecar.c_str(), MOVEFILE_WRITE_THROUGH))
				throw ToolError(500, std::format(
										 "failed to publish capture sidecar '{}': {}",
										 JsonPath(sidecar),
										 std::system_category().message(static_cast<int>(::GetLastError()))));
			remove.path.clear();
		}

		fs::path ResolvePhysical(const CaptureBackend& a_backend, const fs::path& a_path)
		{
			if (!a_backend.resolvePhysicalPath)
				throw ToolError(500,
					"capture physical-path resolution is unavailable; refusing to return a virtual MO2 path");
			try
			{
				auto physical = a_backend.resolvePhysicalPath(a_path);
				if (physical.empty())
					throw std::runtime_error("resolver returned an empty path");
				return fs::absolute(physical).lexically_normal();
			}
			catch (const ToolError&)
			{
				throw;
			}
			catch (const std::exception& error)
			{
				throw ToolError(500, std::format(
										 "failed to resolve physical capture path '{}': {}",
										 JsonPath(a_path), error.what()));
			}
		}

		void MergeSceneSnapshot(json& a_result, const CaptureBackend& a_backend, json& a_degraded)
		{
			if (!a_backend.sceneSnapshot)
				return;
			try
			{
				auto scene = a_backend.sceneSnapshot();
				if (!scene.is_object())
					throw std::runtime_error("scene snapshot was not an object");
				a_result.update(scene);
			}
			catch (const std::exception&)
			{
				AppendDegraded(a_degraded, "sceneSnapshotUnavailable");
			}
		}

		void ComputeInconclusive(json& a_result)
		{
			std::string reason;
			if (a_result.value("sceneMismatch", false))
				reason = "sceneMismatch";
			else
			{
				const auto& degraded = a_result.at("degraded");
				if (!degraded.empty())
				{
					for (const auto& item : degraded)
						reason += (reason.empty() ? "degraded: " : ", ") + item.get<std::string>();
				}
			}
			a_result["inconclusive"] = !reason.empty();
			if (!reason.empty())
				a_result["inconclusiveReason"] = std::move(reason);
		}

		void MaybeScore(json& a_result, const json& a_args, const CaptureBackend& a_backend,
			const fs::path& a_capturePath)
		{
			const auto golden = a_args.find("golden");
			if (golden == a_args.end())
				return;
			fs::path goldenPath(golden->get<std::string>());
			if (goldenPath.is_relative())
				goldenPath = a_backend.gameRoot() / goldenPath;
			try
			{
				goldenPath = ResolvePhysical(a_backend, goldenPath);
			}
			catch (const ToolError& error)
			{
				a_result["goldenError"] = error.what();
				return;
			}
			const auto score = Ssim::ScoreAgainstGolden(a_capturePath, goldenPath, a_args);
			if (!score.ok || !std::isfinite(score.score))
			{
				a_result["goldenError"] = score.error.empty() ?
				                              "golden comparison produced a non-finite score" :
				                              score.error;
				return;
			}
			a_result["ssim"] = score.score;
			a_result["threshold"] = score.threshold;
			a_result["passed"] = score.passed;
			if (!score.regions.empty())
			{
				json regions = json::array();
				for (const auto& region : score.regions)
				{
					regions.push_back(json{
						{ "name", region.name },
						{ "ssim", region.score },
						{ "threshold", region.threshold },
						{ "passed", region.passed },
					});
				}
				a_result["regions"] = std::move(regions);
			}
		}

		void CopyCorrelationFields(json& a_result, const json& a_args)
		{
			for (const auto key : { "runId", "repeat", "atMs", "resolvedAtMs", "resolvedIndex" })
				if (a_args.contains(key))
					a_result[key] = a_args.at(key);
		}

		json BaseResult(const json& a_args, std::string a_provider, const fs::path& a_physicalPath,
			std::uintmax_t a_bytes, int a_width, int a_height, std::string a_requestId,
			std::string a_readyBy, int a_frame, milliseconds a_elapsed)
		{
			json result{
				{ "ok", true },
				{ "provider", std::move(a_provider) },
				{ "kind", "screenshot" },
				{ "path", JsonPath(a_physicalPath) },
				{ "file", a_physicalPath.filename().string() },
				{ "bytes", a_bytes },
				{ "width", a_width },
				{ "height", a_height },
				{ "checkpointId", a_args.at("checkpointId") },
				{ "recording", a_args.value("recording", std::string("adhoc")) },
				{ "variant", a_args.value("variant", std::string("default")) },
				{ "requestId", std::move(a_requestId) },
				{ "frame", a_frame },
				{ "epoch", duration_cast<seconds>(system_clock::now().time_since_epoch()).count() },
				{ "sceneMismatch", a_args.value("sceneMismatch", false) },
				{ "readyBy", std::move(a_readyBy) },
				{ "captureElapsedMs", a_elapsed.count() },
			};
			CopyCorrelationFields(result, a_args);
			return result;
		}

		void PublishAbandoned(EventBus& a_events, std::string_view a_requestId,
			const fs::path& a_path, const json& a_args, std::string_view a_reason)
		{
			a_events.Publish("capture.abandoned", json{
													  { "requestId", a_requestId },
													  { "path", a_path.empty() ? json(nullptr) : json(JsonPath(a_path)) },
													  { "checkpointId", a_args.value("checkpointId", std::string{}) },
													  { "runId", a_args.value("runId", json(nullptr)) },
													  { "reason", a_reason },
												  });
		}
	}

	struct CaptureService::State
	{
		EventBus&            events;
		CaptureConfiguration configuration;
		CaptureBackend       backend;
		std::atomic_uint64_t requestSequence{ 0 };

		State(EventBus& a_events, CaptureConfiguration a_configuration, CaptureBackend a_backend) :
			events(a_events),
			configuration(std::move(a_configuration)),
			backend(std::move(a_backend))
		{}

		std::uint64_t NextSequence()
		{
			return requestSequence.fetch_add(1, std::memory_order_relaxed) + 1;
		}

		std::string RequestId(const json& a_args, std::uint64_t a_sequence) const
		{
			return std::format(
				"{}#{}#{}", a_args.at("checkpointId").get<std::string>(),
				a_args.value("runId", json(nullptr)).dump(), a_sequence);
		}

		int CurrentFrame() const
		{
			return backend.currentFrame ? backend.currentFrame() : -1;
		}

		json Provider(std::string a_provider, const json& a_args, const ToolContext& a_context)
		{
			const auto start = steady_clock::now();
			const auto entry = ToolExtensions::Find("capture", a_provider);
			if (!entry)
				throw ToolError(404, std::format(
										 "no capture provider registered under '{}'", a_provider));

			const auto sequence = NextSequence();
			const auto requestId = RequestId(a_args, sequence);
			const auto directory = CaptureDirectory(configuration, backend, a_args);
			CreateDirectory(directory);
			const auto outputPath = directory / (CaptureStem(a_args) + ".png");
			EnsureNewArtifactPaths(outputPath);

			json providerArgs = a_args;
			providerArgs["outputPath"] = JsonPath(outputPath);
			providerArgs["requestId"] = requestId;
			const auto cursor = events.HeadSeq();

			json queued;
			try
			{
				queued = entry->handler(providerArgs, a_context);
				if (!queued.is_object())
					throw ToolError(502, "capture provider returned a non-object receipt");
				if (const auto ok = queued.find("ok"); ok != queued.end() && !ok->is_boolean())
					throw ToolError(502, "capture provider receipt 'ok' must be boolean");
				if (const auto error = queued.find("error"); error != queued.end() && !error->is_string())
					throw ToolError(502, "capture provider receipt 'error' must be a string");
				if (queued.value("ok", true) == false || queued.contains("error"))
					throw ToolError(502, queued.value(
											 "error", std::string("capture provider rejected the request")));
			}
			catch (const ToolError&)
			{
				PublishAbandoned(events, requestId, outputPath, a_args, "providerDispatchFailed");
				throw;
			}

			const auto      timeout = milliseconds(ReadMilliseconds(
				a_args, "timeoutMs", configuration.timeout, 1, 60000));
			const auto      poll = milliseconds(ReadMilliseconds(
				a_args, "pollMs", milliseconds(100), 10, 1000));
			AwaitedArtifact ready;
			try
			{
				ready = AwaitProviderArtifact(
					events, cursor, requestId, outputPath, timeout, poll);
			}
			catch (const ToolError& error)
			{
				PublishAbandoned(events, requestId, outputPath, a_args, error.what());
				throw;
			}

			const auto          physicalPath = ResolvePhysical(backend, outputPath);
			ArtifactObservation physicalObservation;
			auto                physicalProbe = ProbeArtifact(physicalPath, physicalObservation);
			if (!physicalProbe.complete)
			{
				std::this_thread::sleep_for(poll);
				physicalProbe = ProbeArtifact(physicalPath, physicalObservation);
			}
			if (!physicalProbe.complete)
				throw ToolError(500, physicalProbe.error.empty() ?
										 "physical capture path is not a complete decodable file" :
										 physicalProbe.error);

			auto result = BaseResult(
				a_args, a_provider, physicalPath, physicalProbe.bytes, physicalProbe.width,
				physicalProbe.height, requestId, ready.readyBy, CurrentFrame(),
				duration_cast<milliseconds>(steady_clock::now() - start));
			json degraded = json::array();
			AppendProviderDegraded(degraded, queued);
			for (const auto& reason : ready.providerDegraded)
				AppendDegraded(degraded, reason.get<std::string>());
			if (ready.readyBy == "poll")
				AppendDegraded(degraded, "providerReadyEventMissing");
			if (ready.eventGap)
			{
				result["eventGap"] = *ready.eventGap;
				AppendDegraded(degraded, "eventRingGap");
			}
			const bool excludeUi = a_args.value("excludeUi", true);
			if (excludeUi && !ready.uiExcluded.has_value())
				AppendDegraded(degraded, "uiExclusionUnconfirmed");
			else if (excludeUi && !*ready.uiExcluded)
				AppendDegraded(degraded, "uiIncluded");
			if (a_args.contains("subrect") && !ready.subrectApplied.value_or(false))
				AppendDegraded(degraded, "subrectUnconfirmed");
			if (a_args.value("sceneMismatch", false))
				AppendDegraded(degraded, "sceneMismatch");
			result["uiExcluded"] = ready.uiExcluded.value_or(false);
			result["subrectApplied"] = ready.subrectApplied.value_or(!a_args.contains("subrect"));
			MergeSceneSnapshot(result, backend, degraded);
			result["degraded"] = std::move(degraded);
			ComputeInconclusive(result);
			MaybeScore(result, a_args, backend, physicalPath);
			WriteSidecar(physicalPath, result, sequence);
			events.Publish("capture.saved", result);
			return result;
		}

		json Native(const json& a_args)
		{
			if (!backend.queueNativeScreenshot)
				throw ToolError(503, "native screenshot queueing is unavailable");

			const auto start = steady_clock::now();
			const auto sequence = NextSequence();
			const auto requestId = RequestId(a_args, sequence);
			const auto roots = ScanRoots(configuration, backend);
			const auto before = IndexFiles(ScanDirectories(roots, false));
			if (!backend.queueNativeScreenshot())
				throw ToolError(409,
					"MenuControls::QueueScreenshot did not queue a screenshot (handler unavailable or already queued)");

			const auto  timeout = milliseconds(ReadMilliseconds(
				a_args, "timeoutMs", configuration.timeout, 1, 60000));
			const auto  poll = milliseconds(ReadMilliseconds(
				a_args, "pollMs", milliseconds(100), 10, 1000));
			ScannedFile native;
			try
			{
				native = AwaitNativeArtifact(configuration, backend, before, timeout, poll);
			}
			catch (const ToolError& error)
			{
				PublishAbandoned(events, requestId, {}, a_args, error.what());
				throw;
			}

			const auto destinationDirectory = CaptureDirectory(configuration, backend, a_args);
			CreateDirectory(destinationDirectory);
			const auto destination = destinationDirectory /
			                         (CaptureStem(a_args) + native.path.extension().string());
			EnsureNewArtifactPaths(destination);
			std::error_code ec;
			fs::rename(native.path, destination, ec);
			if (ec)
				throw ToolError(500, std::format(
										 "native screenshot relocation from '{}' to '{}' failed without copying: {}",
										 JsonPath(native.path), JsonPath(destination), ec.message()));

			const auto          physicalPath = ResolvePhysical(backend, destination);
			ArtifactObservation observation;
			auto                probe = ProbeArtifact(physicalPath, observation);
			if (!probe.complete)
			{
				std::this_thread::sleep_for(poll);
				probe = ProbeArtifact(physicalPath, observation);
			}
			if (!probe.complete)
				throw ToolError(500, probe.error.empty() ?
										 "physical native screenshot is not a complete decodable file" :
										 probe.error);

			auto result = BaseResult(
				a_args, "native", physicalPath, probe.bytes, probe.width, probe.height,
				requestId, "poll", CurrentFrame(),
				duration_cast<milliseconds>(steady_clock::now() - start));
			json degraded = json::array({
				"nativeFallback",
				"visualRegressionInconclusive",
				"uiExclusionUnavailable",
				"subrectNotGuaranteed",
			});
			if (a_args.value("sceneMismatch", false))
				AppendDegraded(degraded, "sceneMismatch");
			result["uiExcluded"] = false;
			result["subrectApplied"] = false;
			result["nativeRelocated"] = true;
			MergeSceneSnapshot(result, backend, degraded);
			result["degraded"] = std::move(degraded);
			ComputeInconclusive(result);
			MaybeScore(result, a_args, backend, physicalPath);
			WriteSidecar(physicalPath, result, sequence);
			events.Publish("capture.saved", result);
			return result;
		}
	};

	CaptureConfiguration CaptureConfigurationFromConfig(const Config& a_config)
	{
		CaptureConfiguration configuration{
			.captureDirectory = fs::path(a_config.captureDir),
			.timeout = milliseconds(a_config.captureTimeoutMs),
			.settle = milliseconds(a_config.captureSettleMs),
		};
		for (const auto& directory : a_config.captureScanDirs)
			configuration.scanDirectories.emplace_back(directory);
		return configuration;
	}

	CaptureService::CaptureService(
		EventBus& a_events, CaptureConfiguration a_configuration, CaptureBackend a_backend) :
		state_(std::make_shared<State>(
			a_events, std::move(a_configuration), std::move(a_backend)))
	{
		if (!state_->backend.gameRoot)
			throw std::invalid_argument("capture backend requires gameRoot");
		if (!state_->backend.resolvePhysicalPath)
			throw std::invalid_argument("capture backend requires resolvePhysicalPath");
		if (state_->configuration.scanDirectories.empty())
			state_->configuration.scanDirectories.emplace_back();
	}

	json CaptureService::Handle(const json& a_args, const ToolContext& a_context)
	{
		RequireObject(a_args, "capture");
		const auto kind = LowerAscii(ReadString(a_args, "kind", "auto"));
		if (kind == "providers")
		{
			json providers = json::array();
			for (const auto& key : ToolExtensions::Keys("capture"))
				providers.push_back(key);
			return json{ { "providers", std::move(providers) } };
		}
		if (kind == "extensions")
		{
			json extensions = json::array();
			for (const auto& key : ToolExtensions::Keys("capture"))
			{
				json item{ { "kind", key } };
				if (const auto extension = ToolExtensions::Find("capture", key))
					item["descriptor"] = extension->descriptor;
				extensions.push_back(std::move(item));
			}
			return json{ { "extensions", std::move(extensions) } };
		}

		ValidateCaptureRequest(a_args);
		if (kind == "native")
			return state_->Native(a_args);
		if (kind == "auto")
		{
			const auto providers = ToolExtensions::Keys("capture");
			if (providers.size() == 1)
				return state_->Provider(providers.front(), a_args, a_context);
			if (providers.empty())
			{
				if (a_args.value("allowNative", false))
					return state_->Native(a_args);
				throw ToolError(404,
					"no capture provider is registered; pass kind='native' or allowNative=true "
					"to opt into the inconclusive vanilla fallback");
			}
			std::string names;
			for (const auto& provider : providers)
				names += (names.empty() ? "" : ", ") + provider;
			throw ToolError(400, std::format(
									 "multiple capture providers are registered ({}); pass an explicit kind",
									 names));
		}
		return state_->Provider(ReadString(a_args, "kind"), a_args, a_context);
	}

	json CaptureService::ListScreenshots(const json& a_args) const
	{
		RequireObject(a_args, "screenshot listing");
		int limit = 50;
		if (const auto it = a_args.find("limit"); it != a_args.end())
		{
			if (!it->is_number_integer())
				throw ToolError(400, "'limit' must be an integer");
			limit = it->get<int>();
			if (limit < 0 || limit > kMaxScreenshotListLimit)
				throw ToolError(400, std::format(
										 "'limit' must be between 0 and {}",
										 kMaxScreenshotListLimit));
		}
		std::vector<fs::path> directories;
		if (const auto directory = ReadString(a_args, "dir"); !directory.empty())
		{
			fs::path path(directory);
			if (path.is_relative())
				path = state_->backend.gameRoot() / path;
			directories.push_back(std::move(path));
		}
		else
		{
			directories = ScanRoots(state_->configuration, state_->backend);
		}
		auto files = ScanDirectories(directories, a_args.contains("dir"));
		std::ranges::sort(files, [](const ScannedFile& a_left, const ScannedFile& a_right) {
			if (a_left.modified != a_right.modified)
				return a_left.modified > a_right.modified;
			return PathKey(a_left.path) < PathKey(a_right.path);
		});

		json                  screenshots = json::array();
		std::set<std::string> seen;
		for (const auto& file : files)
		{
			const auto physical = ResolvePhysical(state_->backend, file.path);
			if (!seen.insert(PathKey(physical)).second)
				continue;
			if (static_cast<int>(screenshots.size()) >= limit)
				continue;
			const auto modified = std::chrono::clock_cast<std::chrono::system_clock>(file.modified);
			screenshots.push_back(json{
				{ "file", physical.filename().string() },
				{ "path", JsonPath(physical) },
				{ "bytes", file.size },
				{ "mtimeEpoch", duration_cast<seconds>(modified.time_since_epoch()).count() },
			});
		}
		json roots = json::array();
		for (const auto& directory : directories)
		{
			std::error_code ec;
			if (fs::exists(directory, ec))
				roots.push_back(JsonPath(ResolvePhysical(state_->backend, directory)));
			else if (ec)
				throw ToolError(500, std::format(
										 "failed to inspect screenshot directory '{}': {}",
										 JsonPath(directory), ec.message()));
			else
				roots.push_back(JsonPath(fs::absolute(directory).lexically_normal()));
		}
		return json{
			{ "dirs", std::move(roots) },
			{ "count", seen.size() },
			{ "returned", screenshots.size() },
			{ "truncated", seen.size() > screenshots.size() },
			{ "screenshots", std::move(screenshots) },
		};
	}

	ToolHandler CaptureService::StableHandler()
	{
		const auto weakService = weak_from_this();
		if (weakService.expired())
			throw std::logic_error(
				"CaptureService must be owned by std::shared_ptr before registration");
		return [weakService](const json& a_args, const ToolContext& a_context) {
			const auto service = weakService.lock();
			if (!service)
				throw ToolError(503, "capture service is unavailable");
			return service->Handle(a_args, a_context);
		};
	}

	void CaptureService::Register(ToolRegistry& a_registry)
	{
		a_registry.Register(BuildCaptureDescriptor(), StableHandler());
	}

	void CaptureService::RefreshDescriptor(ToolRegistry& a_registry)
	{
		a_registry.Register(BuildCaptureDescriptor(), StableHandler());
	}

	ToolDescriptor BuildCaptureDescriptor()
	{
		ToolDescriptor descriptor;
		descriptor.name = "capture";
		descriptor.description =
			"Capture a completed decodable screenshot with per-request correlation. kind='auto' "
			"uses the sole registered capture extension, refuses ambiguity, and only uses the "
			"vanilla MenuControls queue when allowNative=true. kind='native' is always degraded "
			"and inconclusive for visual regression because UI exclusion and subrect guarantees "
			"are unavailable; its completed screenshot is moved into the configured capture "
			"directory before publication so RootBuilder cleanup cannot invalidate the result. "
			"kind='providers' lists keys; kind='extensions' includes descriptors; "
			"any other kind names a provider. Providers receive absolute forward-slash outputPath "
			"and requestId, must not overwrite it, and may publish capture.ready with the same "
			"requestId. Success requires an exclusive-open, stable, decodable file, not an event "
			"alone. Optional golden/threshold/regions use the existing SSIM implementation; callers "
			"must reject an inconclusive result even when passed=true.";
		descriptor.readOnly = false;
		json kinds = json::array({ "auto", "native", "providers", "extensions" });
		for (const auto& provider : ToolExtensions::Keys("capture"))
			kinds.push_back(provider);
		descriptor.inputSchema = json{
			{ "type", "object" },
			{ "properties", json{
								{ "kind", json{ { "type", "string" }, { "default", "auto" },
											  { "enum", std::move(kinds) },
											  { "description", "auto | native | providers | extensions | registered provider key" } } },
								{ "checkpointId", json{ { "type", "string" },
													  { "description", "required for capture actions; portable single-segment artifact stem" } } },
								{ "recording", json{ { "type", "string" }, { "default", "adhoc" } } },
								{ "variant", json{ { "type", "string" }, { "default", "default" } } },
								{ "allowNative", json{ { "type", "boolean" }, { "default", false } } },
								{ "excludeUi", json{ { "type", "boolean" }, { "default", true } } },
								{ "outDir", json{ { "type", "string" },
												{ "description", "override the configured canonical output root for provider or native captures" } } },
								{ "cleanup", json{ { "type", "boolean" }, { "default", false },
												 { "description", "accepted for upstream compatibility; native relocation always leaves one canonical artifact" } } },
								{ "timeoutMs", json{ { "type", "integer" }, { "minimum", 1 }, { "maximum", 60000 } } },
								{ "pollMs", json{ { "type", "integer" }, { "minimum", 10 }, { "maximum", 1000 } } },
								{ "subrect", json{ { "type", "object" },
												 { "description", "provider request {x,y,w,h} in normalized 0..1 UV" } } },
								{ "golden", json{ { "type", "string" } } },
								{ "threshold", json{ { "type", "number" }, { "minimum", -1.0 }, { "maximum", 1.0 }, { "default", 0.98 } } },
								{ "regions", json{ { "type", "array" }, { "maxItems", 64 } } },
							} },
		};
		return descriptor;
	}

	json BuildScreenshotInspectionExtensionDescriptor()
	{
		return json{
			{ "description",
				"List image files in configured native screenshot scan directories. Paths are "
				"resolved through open handles to physical files usable outside MO2/USVFS." },
			{ "inputSchema", json{
								 { "type", "object" },
								 { "properties", json{
													 { "kind", json{ { "const", "screenshots" } } },
													 { "dir", json{ { "type", "string" },
																  { "description", "optional absolute path or path relative to the game root" } } },
													 { "limit", json{ { "type", "integer" }, { "minimum", 0 },
																	{ "maximum", kMaxScreenshotListLimit }, { "default", 50 } } },
												 } },
							 } },
		};
	}

	std::shared_ptr<CaptureService> RegisterCaptureTool(
		ToolRegistry& a_registry, EventBus& a_events, CaptureConfiguration a_configuration,
		CaptureBackend a_backend)
	{
		auto service = std::make_shared<CaptureService>(
			a_events, std::move(a_configuration), std::move(a_backend));
		service->Register(a_registry);
		return service;
	}

	void RegisterScreenshotInspectionExtension(
		const std::shared_ptr<CaptureService>& a_service)
	{
		if (!a_service)
			throw std::invalid_argument("screenshot inspection requires a capture service");
		const std::weak_ptr<CaptureService> weakService = a_service;
		ToolExtensions::Register(
			"inspect", "screenshots", BuildScreenshotInspectionExtensionDescriptor(),
			[weakService](const json& a_args, const ToolContext&) {
				const auto service = weakService.lock();
				if (!service)
					throw ToolError(503, "screenshot inspection backend is unavailable");
				return service->ListScreenshots(a_args);
			});
	}
}
