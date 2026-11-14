#include "Form.h"
#include "MainThread.h"
#include "Readers.h"
#include "tools/inspection/InspectLogic.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace dvb::fallout4::inspection
{
	namespace
	{
		using Request = tools::inspection::Request;
		using Budget = tools::inspection::TraversalBudget;

		constexpr std::size_t kNodeBudget = 50000;
		constexpr std::size_t kParentStepBudget = 100000;
		constexpr std::size_t kPathDepth = 64;

		// Suns are held raw at refcount 0; a NiPointer here would free them.
		struct SceneLight
		{
			RE::BSLight*             light;
			std::vector<const char*> lists;
		};

		// One entry per light with every renderer list it is in; build under lightLock.
		class SceneSnapshot
		{
		public:
			explicit SceneSnapshot(RE::ShadowSceneNode& a_scene)
			{
				for (const auto& light : a_scene.nonShadowLights)
					Add("nonShadow", light.get());
				for (const auto& light : a_scene.shadowLights)
					Add("shadow", light.get());
				for (const auto& light : a_scene.specialLights)
					Add("special", light.get());
				Add("sun", a_scene.sunLight);
				Add("cloudSun", a_scene.cloudSunLight);
				Add("directionalShadow", a_scene.directionalShadowLight);
				Add("localSun", a_scene.localSunLight);
			}

			[[nodiscard]] const std::vector<SceneLight>& Lights() const { return lights_; }

			[[nodiscard]] const std::vector<const char*>* ListsOf(const RE::NiLight* a_light) const
			{
				const auto it = byNiLight_.find(a_light);
				return it == byNiLight_.end() ? nullptr : std::addressof(lights_[it->second].lists);
			}

		private:
			void Add(const char* a_list, RE::BSLight* a_light)
			{
				if (!a_light)
					return;
				const auto [it, inserted] = index_.try_emplace(a_light, lights_.size());
				if (inserted)
				{
					lights_.push_back({ a_light, {} });
					if (const auto* source = a_light->GetSourceLight())
						byNiLight_.try_emplace(source, it->second);
				}
				lights_[it->second].lists.push_back(a_list);
			}

			std::vector<SceneLight>                             lights_;
			std::unordered_map<const RE::BSLight*, std::size_t> index_;
			std::unordered_map<const RE::NiLight*, std::size_t> byNiLight_;
		};

		const char* ShapeName(RE::BSLight::Shape a_shape)
		{
			switch (a_shape)
			{
				case RE::BSLight::Shape::kDirectional:
					return "directional";
				case RE::BSLight::Shape::kPoint:
					return "point";
				case RE::BSLight::Shape::kHemisphere:
					return "hemisphere";
				case RE::BSLight::Shape::kBox:
					return "box";
				case RE::BSLight::Shape::kCone:
					return "cone";
				default:
					return "unknown";
			}
		}

		bool CastsShadow(const std::vector<const char*>& a_lists)
		{
			return std::ranges::any_of(a_lists, [](std::string_view a_list) {
				return a_list == "shadow" || a_list == "directionalShadow";
			});
		}

		json ListNames(const std::vector<const char*>* a_lists)
		{
			json out = json::array();
			if (a_lists)
				for (const auto* list : *a_lists)
					out.push_back(list);
			return out;
		}

		// "Root > ... > Parent": names from the top of the graph (or a_root) down to the
		// light's parent; a path cut short starts with "... ".
		std::string NodePath(const RE::NiAVObject& a_object, const RE::NiAVObject* a_root, Budget& a_budget)
		{
			std::vector<std::string> names;
			bool                     cut = false;
			for (const RE::NiAVObject* node = a_object.parent; node; node = node->parent)
			{
				if (names.size() >= kPathDepth || !a_budget.StepParent())
				{
					cut = true;
					break;
				}
				const char* name = node->name.c_str();
				names.emplace_back(name && *name ? name : "(unnamed)");
				if (node == a_root)
					break;
			}
			std::string path;
			for (auto it = names.rbegin(); it != names.rend(); ++it)
				path += (path.empty() ? "" : " > ") + *it;
			return cut ? "... " + path : path;
		}

		json DescribeNiLight(const RE::NiLight& a_light, const RE::NiAVObject* a_root, Budget& a_budget)
		{
			const auto  position = a_light.world.translate;
			const char* name = a_light.name.c_str();
			const auto* rtti = a_light.GetRTTI();
			json        out{
				{ "name", name ? name : "" },
				{ "type", rtti && rtti->GetName() ? rtti->GetName() : "" },
				{ "path", NodePath(a_light, a_root, a_budget) },
				{ "position", tools::inspection::Vec3Json(position.x, position.y, position.z) },
				{ "diffuse", json::array({ a_light.diff.r, a_light.diff.g, a_light.diff.b }) },
				{ "dimmer", a_light.dimmer },
				{ "fadeAmount", a_light.fadeAmount },
				{ "appCulled", a_light.GetAppCulled() },
			};
			if (const auto* directional = netimmerse_cast<const RE::NiDirectionalLight*>(&a_light))
				out["direction"] = tools::inspection::Vec3Json(
					directional->direction.x, directional->direction.y, directional->direction.z);
			else
				out["radius"] = a_light.GetRadius();
			return out;
		}

		void AddSceneFields(json& a_out, const RE::BSLight& a_light, const std::vector<const char*>& a_lists)
		{
			a_out["lists"] = ListNames(std::addressof(a_lists));
			a_out["shape"] = ShapeName(a_light.GetShape());
			a_out["castsShadow"] = CastsShadow(a_lists);
			if (a_light.GetShape() == RE::BSLight::Shape::kCone)
				a_out["cone"] = json{
					{ "exponent", a_light.cone.exponent },
					{ "cosineCutoff", a_light.cone.cosineCutoff },
				};
		}

		// Every NiLight under a_root, each node charged to the budget; a node reached twice
		// is read once.
		json LightsUnder(
			const RE::NiAVObject*                      a_root,
			const SceneSnapshot&                       a_scene,
			Budget&                                    a_budget,
			std::unordered_set<const RE::NiAVObject*>& a_visited)
		{
			json                               out = json::array();
			std::vector<const RE::NiAVObject*> stack;
			if (a_root)
				stack.push_back(a_root);
			while (!stack.empty())
			{
				const auto* object = stack.back();
				stack.pop_back();
				if (!a_visited.insert(object).second)
					continue;
				if (!a_budget.VisitNode())
					break;
				if (const auto* light = netimmerse_cast<const RE::NiLight*>(object))
				{
					if (!a_budget.Emit())
						break;
					auto        described = DescribeNiLight(*light, a_root, a_budget);
					const auto* lists = a_scene.ListsOf(light);
					described["lists"] = ListNames(lists);
					if (lists)
						described["castsShadow"] = CastsShadow(*lists);
					out.push_back(std::move(described));
				}
				if (const auto* node = object->IsNode())
				{
					const auto& children = node->children;
					for (auto i = children.size(); i > 0; --i)
						if (const auto& child = children[static_cast<std::uint32_t>(i - 1)])
							stack.push_back(child.get());
				}
			}
			return out;
		}

		json Truncation(const Budget& a_budget)
		{
			json by = json::array();
			for (const auto& name : a_budget.TruncatedBy())
				by.push_back(name);
			return json{ { "truncated", a_budget.Truncated() }, { "truncatedBy", std::move(by) } };
		}

		json ReadSceneLights(const Request& a_request, const SceneSnapshot& a_scene)
		{
			auto*      player = RE::PlayerCharacter::GetSingleton();
			const bool havePosition = player && player->Get3D();
			if (a_request.radius > 0.0 && !havePosition)
				throw ToolError(409, "inspect lights: 'radius' needs a loaded player position");
			const auto origin = havePosition ? player->GetPosition() : RE::NiPoint3{};

			Budget                                     budget(kNodeBudget, kParentStepBudget, a_request.limit);
			std::vector<tools::inspection::SceneEntry> entries;
			for (std::size_t i = 0; i < a_scene.Lights().size(); ++i)
			{
				const auto* source = a_scene.Lights()[i].light->GetSourceLight();
				if (!source)
					continue;
				const auto           position = source->world.translate;
				std::optional<float> distance;
				if (havePosition)
					distance = origin.GetDistance(position);
				if (a_request.radius > 0.0 && *distance > a_request.radius)
					continue;
				const char* name = source->name.c_str();
				entries.push_back({ distance, name ? name : "", {}, position.x, position.y, position.z, i });
			}
			tools::inspection::OrderScene(entries);

			json lights = json::array();
			for (const auto& entry : entries)
			{
				if (!budget.Emit())
					break;
				const auto& scene = a_scene.Lights()[entry.index];
				auto        described = DescribeNiLight(*scene.light->GetSourceLight(), nullptr, budget);
				AddSceneFields(described, *scene.light, scene.lists);
				if (entry.distance)
					described["distance"] = *entry.distance;
				lights.push_back(std::move(described));
			}
			json out{
				{ "scope", "scene" },
				{ "playerPosition", havePosition },
				{ "count", entries.size() },
				{ "returned", lights.size() },
			};
			out.update(Truncation(budget));
			out["lights"] = std::move(lights);
			return out;
		}

		json ReadReferenceLights(const Request& a_request, const SceneSnapshot& a_scene)
		{
			auto*                            player = RE::PlayerCharacter::GetSingleton();
			RE::NiPointer<RE::TESObjectREFR> picked;
			RE::TESObjectREFR*               reference = nullptr;
			if (a_request.selected)
			{
				picked = RE::Console::GetCurrentPickREFR().get();
				reference = picked.get();
				if (!reference)
					throw ToolError(404, "inspect lights: no reference is selected");
			}
			else
			{
				reference = ResolveReferenceTarget(a_request.formId, player, "lights");
			}

			Budget                                    budget(kNodeBudget, kParentStepBudget, a_request.limit);
			std::unordered_set<const RE::NiAVObject*> visited;
			const auto*                               root = reference->Get3D();
			json                                      out{
				{ "scope", "ref" },
				{ "ref", SerializeForm(reference) },
				{ "loaded3D", root != nullptr },
			};
			out["lights"] = LightsUnder(root, a_scene, budget, visited);
			if (reference == player)
				out["firstPerson"] = LightsUnder(player->firstPerson3D.get(), a_scene, budget, visited);
			out.update(Truncation(budget));
			return out;
		}
	}

	json ReadLights(const Request& a_request)
	{
		const auto scope = tools::inspection::ParseLightsScope(a_request.scope);
		if (!scope)
			throw ToolError(
				400, std::format("inspect lights: unknown scope '{}' (ref|scene)", a_request.scope));
		return MainThread::RunAndWait([request = a_request, scope = *scope]() -> json {
			RequireStableWorld("lights");
			auto* state = RE::BSShaderManager::State::GetSingleton();
			auto* scene = state ? state->GetShadowSceneNode(
									  static_cast<std::size_t>(RE::BSShaderManager::eSceneGraphEnum::kWorld)) :
			                      nullptr;
			if (!scene)
				throw ToolError(503, "inspect lights: the world scene is unavailable");
			if (scope == tools::inspection::LightsScope::kScene)
			{
				// Scene lights are dereferenced, so the lock spans the whole read.
				const RE::BSAutoLock lock(scene->lightLock);
				return ReadSceneLights(request, SceneSnapshot(*scene));
			}
			// Ref scope only compares light addresses against the snapshot.
			const auto snapshot = [scene] {
				const RE::BSAutoLock lock(scene->lightLock);
				return SceneSnapshot(*scene);
			}();
			return ReadReferenceLights(request, snapshot);
		});
	}
}
