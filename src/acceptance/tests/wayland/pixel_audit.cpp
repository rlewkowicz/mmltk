#include "audit_facts.h"
#include "pixel_audit.h"
#include "src/controller/presentation/workspace_presentation_types.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
namespace mmltk::acceptance::wayland {
auto PixelBoundaryAudit::probe_failure_evidence(std::string_view expected) const -> ProbeFailureEvidence {
        if (expected.empty() || !failure.empty() || probe_failures.size() != 1U) return {};
        const auto& failed = probe_failures.front();
        std::string boundary{expected};
        boundary.front() = static_cast<char>(std::toupper(static_cast<unsigned char>(boundary.front())));
        if (failed.value("boundary", "") != boundary) return {};
        const auto source = failed.value("source", "");
        if (!SurfaceAudit::valid_identity(source)) return {};
        const bool allocation = expected == "allocation";
        bool forwarded = false, recovered = false;
        for (const auto& [key, publication] : samples) {
            if (publication.forwarded.empty()) continue;
            const auto& receipt = publication.receivers[0].identity;
            const bool same_source = publication.forwarded.value("workspace_source", "") == source;
            if (same_source) {
                for (const auto receiver : {0U, 1U}) {
                    const auto& evidence = publication.receivers[receiver].identity;
                    if (!evidence.empty() && (allocation || scalar(evidence, "transfer_sequence") == scalar(failed, "transfer_sequence"))) return {};
                }
                const bool same_attempt = key.second == scalar(failed, "presentation_revision") &&
                                          scalar(publication.forwarded, "transfer_sequence") == scalar(failed, "transfer_sequence");
                if (!publication.forwarded.empty() && (allocation || same_attempt)) {
                    if (publication.forwarded.value("pixel_probe", true)) return {};
                    if (!allocation) {
                        for (const auto* field : {"layer", "slot", "content_session", "content_sequence"})
                            if (scalar(publication.forwarded, field) != scalar(failed, field)) return {};
                    }
                    forwarded = true;
                }
            }
            // The failed import may be retired and replaced. Correlate recovery
            // by the continuing logical content stream rather than requiring
            // the replacement to reuse a physical surface or mailbox slot.
            if (!allocation && !receipt.empty() && scalar(receipt, "layer") == scalar(failed, "layer") &&
                scalar(receipt, "content_session") == scalar(failed, "content_session") &&
                scalar(receipt, "content_sequence") == scalar(failed, "content_sequence") &&
                scalar(receipt, "presentation_revision") > scalar(failed, "presentation_revision") &&
                std::ranges::all_of(publication.counted, [](bool value) { return value; }))
                recovered = true;
            if (allocation && !same_source && !receipt.empty() && scalar(receipt, "presentation_revision") > scalar(failed, "presentation_revision") &&
                std::ranges::all_of(publication.counted, [](bool value) { return value; }))
                recovered = true;
        }
        return {.forwarded = forwarded || (allocation && failed_probe_retirements.contains(source)), .recovered = recovered};
    }

auto PixelBoundaryAudit::copy_probe_omission_proven() const -> bool {
        return failure.empty() && probe_failures.empty() && direct_joined != 0U && std::ranges::all_of(samples, [](const auto& item) {
                   const auto& publication = item.second;
                   return publication.forwarded.empty() ||
                          (publication.direct() && publication.receivers[0].identity.empty() && publication.receivers[1].identity.empty());
               });
    }

auto PixelBoundaryAudit::probe_failure_complete(std::string_view expected) const -> bool {
        if (expected.empty()) return probe_failures.empty();
        // The injected Firefox probe boundary exists only on the capability
        // copy route. Direct hardware proves its omission and the real sampled
        // pixels instead of inventing an allocation/reset/copy failure.
        if (copy_probe_omission_proven()) return raw_complete() && viewer_nonblack_complete();
        return probe_failure_evidence(expected).complete() && raw_complete() && viewer_nonblack_complete();
    }

auto PixelBoundaryAudit::consume_continuity(const nlohmann::json& record, std::string_view event) -> void {
        if (!enabled) return;
        const auto control = textual(record, "control");
        const auto detail = textual(record, "detail");
        if (event == "integration.viewer_departure_started") {
            navigation_stage = 1U;
            route_persistence = scalar(record, "b") != 0U;
            route_revision = scalar(record, "c");
            authoritative_route = false;
        } else if (event == "integration.navigation_message" && (navigation_stage == 1U || navigation_stage == 5U)) {
            const bool train = navigation_stage == 1U;
            if (control != (train ? "navigation.train" : "navigation.explore") || detail != (train ? "Explore" : "Train"))
                reject("wrong mapped navigation message path");
            else
                ++navigation_stage;
        } else if (event == "integration.navigation_outcome" && navigation_stage > 0U && navigation_stage < 8U) {
            const bool train = navigation_stage == 2U;
            if ((!train && navigation_stage != 6U) || control != (train ? "navigation.train" : "navigation.explore") || detail != (train ? "Train" : "Explore"))
                reject("navigation outcome lacks its mapped root/router message");
            else
                ++navigation_stage;
        } else if (event == "integration.route_state" && (navigation_stage == 3U || navigation_stage == 7U)) {
            if ((detail == "settings.reply" || detail == "settings.event") && scalar(record, "a") == 1U &&
                control == (navigation_stage == 3U ? "navigation.train" : "navigation.explore"))
                authoritative_route = true;
        } else if (event == "integration.viewer_route_confirmed") {
            const bool train = navigation_stage == 3U;
            if ((!train && navigation_stage != 7U) || control != (train ? "navigation.train" : "navigation.explore") ||
                (train ? detail != "None" : (detail != "Explore" && detail != "Upscale")) || scalar(record, "c") != 1U ||
                (scalar(record, "b") != 0U) != route_persistence || (route_persistence && (!authoritative_route || scalar(record, "a") <= route_revision))) {
                reject("mapped route foreground or authoritative persistence is incomplete");
            } else {
                ++navigation_stage;
                route_revision = scalar(record, "a");
                authoritative_route = false;
            }
        } else if (event == "integration.viewer_abandoned") {
            if (navigation_stage != 4U)
                reject("viewer abandoned before mapped departure confirmation");
            else
                navigation_stage = 5U;
        } else if (event == "integration.viewer_basic_reentry") {
            if (navigation_stage != 8U || scalar(record, "a") == 0U || scalar(record, "b") == 0U)
                reject("automatic Basic lacks mapped reentry and completed draw");
            else {
                reentry_product = {scalar(record, "a"), scalar(record, "c"), scalar(record, "d")};
                navigation_stage = 9U;
            }
        } else if (event == "integration.viewer_reconnected") {
            if (navigation_stage != 9U || scalar(record, "b") == 0U ||
                reentry_product != std::array{scalar(record, "a"), scalar(record, "c"), scalar(record, "d")})
                reject("reconnect did not restore the same completed viewer product");
            else
                navigation_stage = 10U;
            successful_viewer = std::pair{scalar(record, "b"), scalar(record, "a")};
        } else if (event == "integration.explore_reopened" && navigation_stage == 10U) {
            if (detail == "usable-after-reopen" && scalar(record, "c") > 0U && scalar(record, "d") > 0U) navigation_stage = 11U;
        }
        if (event == "integration.viewer_complete" && detail != "copy") successful_viewer = std::pair{scalar(record, "a"), scalar(record, "b")};
    }

auto PixelBoundaryAudit::raw_complete() const -> bool {
        return failure.empty() && std::ranges::any_of(samples, [](const auto& entry) { return entry.second.complete(); });
    }

auto PixelBoundaryAudit::viewer_nonblack_complete() const -> bool {
        if (!successful_viewer || !failure.empty()) return false;
        return std::ranges::any_of(samples, [&](const auto& entry) {
            const auto& publication = entry.second;
            if (entry.first.second != successful_viewer->first || !publication.viewer || !publication.complete() ||
                scalar(publication.receivers[3].identity, "content_sequence") != successful_viewer->second)
                return false;
            const auto has_color = [](const auto& boundary) {
                return std::ranges::any_of(boundary.values, [](const auto& value) { return value && colored(value->rgba); });
            };
            const auto& canvas = publication.receivers[3];
            const auto& imported = publication.receivers[0];
            const auto& mailbox = publication.receivers[1];
            const auto& owned = publication.receivers[2];
            const auto attempt =
                publication.native.find({publication.forwarded.value("workspace_source", ""), scalar(publication.forwarded, "transfer_sequence")});
            const bool canvas_sampled = std::ranges::any_of(canvas.values, [](const auto& value) { return value.has_value(); });
            if (attempt == publication.native.end() || (!publication.direct() && (!imported.complete() || !mailbox.complete())) || !owned.complete() ||
                !canvas_sampled || canvas.identity != owned.identity)
                return false;
            const auto& native = attempt->second;
            return native.complete() && !native.publication_fact.empty() && native.identity == native.publication_fact &&
                   scalar(native.identity, "source_revision") == successful_viewer->second && has_color(native) && has_color(canvas);
        });
    }

auto PixelBoundaryAudit::colored(std::uint32_t value) -> bool {
        return (value >> 24U) != 0U && ((value & 255U) > 8U || ((value >> 8U) & 255U) > 8U || ((value >> 16U) & 255U) > 8U);
    }

auto PixelBoundaryAudit::black(std::uint32_t value) -> bool { return (value & 0x00ffffffU) == 0U; }

auto PixelBoundaryAudit::reject(std::string_view reason) -> void {
        if (failure.empty()) failure = reason;
    }

auto PixelBoundaryAudit::identity_of(const nlohmann::json& record, std::size_t boundary) -> nlohmann::json {
        nlohmann::json identity = nlohmann::json::object();
        identity["direct_sampling"] = record.value("direct_sampling", false);
        const auto copy = [&](const char* field) { identity[field] = scalar(record, field); };
        for (const auto* field : {"presentation_revision"}) copy(field);
        if (boundary == 0U) {
            identity["workspace_source"] = SurfaceAudit::native_identity(record, true);
            copy("workspace_allocation");
            for (const auto* field : {"source_session", "source_instance", "source_revision", "clean_revision", "source_observation_revision", "source_width",
                                      "source_height", "content_x", "content_y", "content_width", "content_height", "allocation_generation", "capacity_width",
                                      "capacity_height", "transfer_sequence", "timeline_ready"})
                copy(field);
        } else {
            for (const auto* field : {"content_session", "content_width", "content_height", "layer", "slot"}) copy(field);
            identity["content_sequence"] = scalar(record, boundary < 3U ? "content_sequence" : "frame_revision");
            if (boundary < 3U) {
                for (const auto* field : {"transfer_sequence", "timeline_ready", "timeline_release"}) copy(field);
            } else {
                identity["workspace_source"] = record.value("source", "");
                for (const auto* field : {"width", "height"}) copy(field);
            }
        }
        return identity;
    }

auto PixelBoundaryAudit::reconcile(Publication& publication) -> void {
        const auto& imported = publication.receivers[0];
        if (publication.forwarded.empty()) return;
        const bool direct = publication.direct();
        if (!direct && imported.identity.empty()) return;
        if (direct && (!imported.identity.empty() || !publication.receivers[1].identity.empty()))
            reject("direct sampling produced a forbidden import or sample-arena pixel copy");
        const auto& acquired = direct ? publication.forwarded : imported.identity;
        for (const auto* field : {"content_session", "content_sequence", "presentation_revision", "content_width", "content_height", "layer", "slot",
                                  "transfer_sequence", "timeline_ready", "timeline_release"})
            if (scalar(acquired, field) != scalar(publication.forwarded, field)) reject("Firefox pixel receipt differs from the forwarded physical mailbox");
        if (scalar(acquired, "layer") != static_cast<std::uint64_t>(mmltk::controller::presentation::WorkspacePresentationLayer::Primary))
            reject("Firefox pixel receipt does not name the native Primary layer");
        const auto native = publication.native.find({publication.forwarded.value("workspace_source", ""), scalar(acquired, "transfer_sequence")});
        if (native == publication.native.end()) return;
        const auto& source = native->second;
        const auto& fact = source.identity;
        if (fact.empty()) return;
        // Independent streams can arrive in either order. Settlement requires
        // the native publication, even after a complete receiver probe batch.
        if (source.publication_fact.empty()) return;
        if (fact != source.publication_fact) reject("native pixels differ from the canonical publication fact");
        if (fact.value("direct_sampling", false) != direct) reject("native and acquired sampling modes differ");
        if (fact.value("workspace_source", "") != publication.forwarded.value("workspace_source", "")) reject("pixel bridge names a different producer source");
        const auto width = scalar(fact, "source_width"), height = scalar(fact, "source_height");
        if (width == 0U || height == 0U || width > scalar(fact, "capacity_width") || height > scalar(fact, "capacity_height") ||
            scalar(fact, "source_session") == 0U || scalar(fact, "source_instance") == 0U || scalar(fact, "source_revision") == 0U ||
            scalar(fact, "clean_revision") == 0U || scalar(fact, "source_observation_revision") == 0U || scalar(fact, "allocation_generation") == 0U ||
            scalar(fact, "content_x") + scalar(fact, "content_width") > width || scalar(fact, "content_y") + scalar(fact, "content_height") > height ||
            scalar(fact, "timeline_ready") != scalar(fact, "transfer_sequence") * 2U - 1U) {
            reject("invalid native product, content, allocation, or transfer fact");
            return;
        }
        std::array<const Boundary*, 4> raw{&source, &publication.receivers[0], &publication.receivers[1], &publication.receivers[2]};
        for (std::size_t owner = 1U; owner < raw.size(); ++owner) {
            if (direct && owner < 3U) continue;
            const auto& identity = raw[owner]->identity;
            if (identity.empty()) continue;
            if (scalar(identity, "content_session") != scalar(fact, "source_session") ||
                scalar(identity, "content_sequence") != scalar(fact, "source_revision") || scalar(identity, "content_width") != width ||
                scalar(identity, "content_height") != height || scalar(identity, "layer") != scalar(acquired, "layer") ||
                scalar(identity, "slot") != scalar(acquired, "slot") || scalar(identity, "layer") >= 3U || scalar(identity, "slot") >= 2U)
                reject("receiver content or physical mailbox identity differs");
            if (identity.value("direct_sampling", false) != direct) reject("pixel sample mode differs from its acquisition");
            if (owner < 3U && (scalar(identity, "transfer_sequence") != scalar(fact, "transfer_sequence") ||
                               scalar(identity, "timeline_ready") != scalar(fact, "timeline_ready") ||
                               scalar(identity, "timeline_release") != scalar(fact, "timeline_ready") + 1U))
                reject("Firefox physical timeline receipt differs");
            if (owner == 3U && (identity.value("workspace_source", "") != fact.value("workspace_source", "") ||
                                scalar(identity, "width") != scalar(fact, "capacity_width") || scalar(identity, "height") != scalar(fact, "capacity_height")))
                reject("Iced allocation differs from native publication");
        }
        const auto coordinate = [](std::size_t index, std::uint64_t size) {
            return std::array<std::uint64_t, 5>{0U, std::min(191UL, size - 1U), std::min(383UL, size - 1U), (size - 1U) / 2U, size - 1U}[index];
        };
        for (std::size_t owner = 0U; owner < raw.size(); ++owner) {
            if (direct && owner > 0U && owner < 3U) continue;
            const auto previous = direct ? 0U : owner - 1U;
            for (std::size_t index = 0U; index < 25U; ++index) {
                const auto& sample = raw[owner]->values[index];
                if (!sample) continue;
                if (sample->x != coordinate(index % 5U, width) || sample->y != coordinate(index / 5U, height))
                    reject("raw probe coordinate differs from logical-content multiset");
                if (owner != 0U && raw[previous]->values[index] && sample != raw[previous]->values[index]) reject("ordered raw RGBA or alpha divergence");
            }
            if (owner != 0U && !(direct ? publication.direct_counted : publication.counted[owner - 1U]) && raw[previous]->complete() &&
                raw[owner]->complete() && failure.empty()) {
                if (direct) {
                    publication.direct_counted = true;
                    ++direct_joined;
                } else {
                    publication.counted[owner - 1U] = true;
                    ++joined[owner - 1U];
                }
            }
        }
        if (publication.viewer) {
            const auto& canvas = publication.receivers[3];
            if (!canvas.identity.empty() && !raw[3]->identity.empty() && canvas.identity != raw[3]->identity)
                reject("canvas does not name the sampled physical publication");
            for (std::size_t index = 0U; index < 25U; ++index) {
                if (canvas.values[index] &&
                    (canvas.values[index]->x != coordinate(index % 5U, width) || canvas.values[index]->y != coordinate(index / 5U, height)))
                    reject("canvas probe does not name its logical source sample");
                if (canvas.values[index] && raw[3]->values[index] && colored(raw[3]->values[index]->rgba) && black(canvas.values[index]->rgba))
                    reject("completed sample produced an unexplained black viewer canvas");
            }
        }
        if (publication.complete()) {
            // Automatic Basic may supersede the raw detail before display.
            // Any completely joined logical crop proves high-water sampling.
            if (width < scalar(fact, "capacity_width") || height < scalar(fact, "capacity_height")) retained_logical_content = true;
            if (width == 1536U && height == 1536U) upscale_growth = true;
        }
    }

auto PixelBoundaryAudit::consume(const nlohmann::json& record) -> void {
        const std::string event = record.value("event", "");
        consume_continuity(record, event);
        if (event == "firefox.workspace.probe_failed") {
            if (!enabled || probe_failures.size() >= 16U)
                reject("unexpected or excessive probe preparation failures");
            else
                probe_failures.push_back(record);
            return;
        }
        if (event == "firefox.workspace.source.retired") {
            const auto surface = record.value("surface", "");
            if (!surface.empty() && std::ranges::any_of(probe_failures, [&](const auto& failed) { return failed.value("source", "") == surface; }))
                failed_probe_retirements.insert(surface);
            return;
        }
        if (event == "upscale.stop.requested") {
            if (stop_observations.size() < kAcceptanceRecordLimit)
                stop_observations.push_back(scalar(record, "observation_revision"));
            else
                reject("Stop observations exceeded bounded acceptance capacity");
        }
        if (event == "integration.upscale_cached") {
            const auto method = scalar(record, "c");
            if (method < 3U)
                cached_methods.insert(method);
            else
                reject("unknown cached Upscale method");
        }
        if (event == "integration.viewer_departure_started") departure_begin = scalar(record, "a");
        if (event == "integration.viewer_abandoned") departure_end = scalar(record, "a");
        if (event == "integration.viewer_settings_preserved") settings_preserved = true;
        if (event == "integration.viewer_basic_reentry") basic_reentry = true;
        if (event == "integration.viewer_reconnected") reconnected = true;
        if (event == "integration.atlas_composition" || event == "integration.atlas_composition_complete") {
            const CompositionKey key{scalar(record, "columns"), record.value("surface", ""), scalar(record, "presentation_revision")};
            if ((std::get<0>(key) != 4U && std::get<0>(key) != 10U) || std::get<1>(key).empty() || std::get<2>(key) == 0U) {
                reject("invalid composition publication or columns");
                return;
            }
            if (!compositions.contains(key) && compositions.size() >= kAcceptanceRecordLimit) {
                reject("composition evidence exceeded bounded publication capacity");
                return;
            }
            auto& composition = compositions[key];
            const auto identity = identity_of(record, 3U);
            if (!composition.identity.empty() && composition.identity != identity)
                reject("composition mixed physical publications");
            else
                composition.identity = identity;
            if (event == "integration.atlas_composition_complete") {
                if (!record.contains("cards") || !record["cards"].is_array() || record["cards"].empty() || record["cards"].size() > 256U) {
                    reject("invalid expected composition card set");
                    return;
                }
                std::set<std::uint64_t> expected;
                for (const auto& card : record["cards"]) expected.insert(card.get<std::uint64_t>());
                if (expected.size() != record["cards"].size() || scalar(record, "emitted") != expected.size() * 4U ||
                    (composition.summary && composition.expected != expected))
                    reject("conflicting or incomplete composition summary");
                composition.expected = std::move(expected);
                composition.summary = true;
                for (const auto& [card, values] : composition.cards)
                    if (!composition.expected.contains(card)) reject("unexpected composition card");
            } else {
                const auto card = scalar(record, "card"), kind = scalar(record, "kind");
                if (kind >= 4U || (!composition.cards.contains(card) && composition.cards.size() >= 256U)) {
                    reject("composition card or kind exceeds bounded contract");
                    return;
                }
                if (composition.summary && !composition.expected.contains(card)) reject("extra composition card");
                const auto valid_channels = [&](const char* field) {
                    return record.contains(field) && record[field].is_array() && record[field].size() == 4U &&
                           std::ranges::all_of(record[field], [](const auto& value) {
                               return value.is_number() && value.template get<double>() >= 0.0 && value.template get<double>() <= 255.0;
                           });
                };
                if (!valid_channels("expected") || !valid_channels("observed")) {
                    reject("invalid composition RGBA arrays");
                    return;
                }
                for (std::size_t channel = 0U; channel < 4U; ++channel)
                    if (std::abs(record["expected"][channel].get<double>() - record["observed"][channel].get<double>()) > 4.0)
                        reject("deterministic composition RGBA differs");
                for (const auto* axis : {"x", "y"}) {
                    const std::string sample_field = std::string{"sample_"} + axis;
                    const std::string canvas_field = std::string{"canvas_"} + axis;
                    const std::string origin_field = std::string{"image_"} + axis;
                    const auto dimension = std::string_view{axis} == "x" ? "width" : "height";
                    const auto content = std::string_view{axis} == "y" && scalar(record, "rows") != 0U && scalar(record, "card_extent") != 0U
                                             ? scalar(record, "rows") * scalar(record, "card_extent")
                                             : scalar(record, (std::string{"content_"} + dimension).c_str());
                    const auto extent = record.value(std::string{"image_"} + dimension, 0.0);
                    const auto sample = record.value(sample_field, -1.0);
                    const auto canvas = record.value(canvas_field, -1.0);
                    if (content == 0U || extent <= 0.0 || !std::isfinite(canvas) || sample < 0.0 || sample >= static_cast<double>(content) ||
                        std::abs(canvas - (record.value(origin_field, 0.0) + sample * extent / static_cast<double>(content))) > 0.01)
                        reject("composition transformed canvas coordinate differs");
                }
                auto& value = composition.cards[card][kind];
                auto stable_record = record;
                stable_record.erase("elapsed_ms");
                if (value && *value != stable_record)
                    reject("duplicate-conflicting composition card sample");
                else
                    value = std::move(stable_record);
            }
            return;
        }
        std::size_t boundary = 0U;
        if (event == "presentation.pixel" || event == "presentation.frame.edge")
            boundary = 0U;
        else if (event == "firefox.workspace.frame_forwarded")
            boundary = 1U;
        else if (event == "firefox.workspace.pixel") {
            const auto owner = record.value("boundary", "");
            if (owner != "import" && owner != "mailbox") {
                reject("unknown Firefox pixel owner");
                return;
            }
            boundary = owner == "import" ? 1U : 2U;
        } else if (event == "iced.surface.pixel")
            boundary = 3U;
        else if (event == "iced.surface.canvas_pixel") {
            canvas_seen = true;
            if (record.value("control", "") != "explore.detail.workspace") return;
            boundary = 4U;
        } else
            return;
        if (!enabled && (event == "presentation.frame.edge" || event == "firefox.workspace.frame_forwarded")) return;
        const std::string surface = boundary == 0U ? SurfaceAudit::native_identity(record) : record.value("surface", "");
        const Key key{surface, scalar(record, "presentation_revision")};
        if (!SurfaceAudit::valid_identity(surface) || key.second == 0U) {
            reject("invalid physical pixel publication");
            return;
        }
        if (!samples.contains(key) && samples.size() >= kAcceptanceRecordLimit) {
            if (failure.empty()) failure = "pixel evidence exceeded its bounded acceptance capacity";
            return;
        }
        auto& publication = samples[key];
        if (event == "firefox.workspace.frame_forwarded") {
            auto identity = identity_of(record, 1U);
            identity["workspace_source"] = record.value("source", "");
            if (!SurfaceAudit::valid_identity(identity["workspace_source"].get<std::string>())) {
                reject("forwarded pixel bridge omitted exact producer source");
                return;
            }
            identity["pixel_probe"] = record.value("pixel_probe", false);
            if (!publication.forwarded.empty() && publication.forwarded != identity)
                reject("forwarded physical mailbox changed within a publication");
            else
                publication.forwarded = identity;
            reconcile(publication);
            return;
        }
        const auto transfer = std::pair{SurfaceAudit::native_identity(record, true), scalar(record, "transfer_sequence")};
        if (boundary == 0U && (!SurfaceAudit::valid_identity(transfer.first) || scalar(record, "workspace_allocation") == 0U)) {
            reject("native pixel fact omitted exact workspace allocation");
            return;
        }
        if (boundary == 0U && !publication.native.contains(transfer) && publication.native.size() >= 16U) {
            reject("native publication reoffers exceeded bounded evidence capacity");
            return;
        }
        auto& target = boundary == 0U ? publication.native[transfer] : publication.receivers[boundary - 1U];
        const auto identity = identity_of(record, boundary);
        if (event == "presentation.frame.edge") {
            target.publication_fact = identity;
            reconcile(publication);
            return;
        }
        if (!target.identity.empty() && target.identity != identity)
            reject("immutable owner identity changed");
        else
            target.identity = identity;
        const auto index = scalar(record, "sample_index");
        if (index >= 25U) {
            reject("pixel index exceeds fixed sample contract");
            return;
        }
        const Sample value{scalar(record, "sample_x"), scalar(record, "sample_y"), static_cast<std::uint32_t>(scalar(record, "sample_rgba"))};
        if (boundary != 4U && target.values[index] && target.values[index] != value) reject("immutable owner pixel changed");
        target.values[index] = value;
        publication.viewer = publication.viewer || boundary == 4U;
        if (publication.viewer && boundary == 4U) ++viewer_canvas_joins;
        reconcile(publication);
    }

auto PixelBoundaryAudit::composition_complete() const -> bool {
        return failure.empty() && std::ranges::all_of(std::array{4U, 10U}, [&](const auto columns) {
                   return std::ranges::any_of(compositions, [&](const auto& value) { return std::get<0>(value.first) == columns && value.second.complete(); });
               });
    }

auto PixelBoundaryAudit::continuity_complete(bool require_gallery ) const -> bool {
        return failure.empty() && (!enabled || navigation_stage >= (require_gallery ? 11U : 10U)) && cached_methods.size() == 3U && settings_preserved &&
               basic_reentry && reconnected && departure_begin != 0U && departure_end > departure_begin &&
               std::ranges::count_if(stop_observations, [&](const auto revision) { return revision >= departure_begin && revision < departure_end; }) == 1;
    }
} // namespace mmltk::acceptance::wayland
