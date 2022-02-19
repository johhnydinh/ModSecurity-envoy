#include <string>
#include <vector>
#include <iostream>

#include "http_filter.h"
#include "utility.h"

#include "absl/container/fixed_array.h"
#include "common/http/utility.h"
#include "common/http/headers.h"
#include "common/config/metadata.h"
#include "envoy/server/filter_config.h"
#include "common/json/json_loader.h"
#include "modsecurity/rule_message.h"
#include "modsecurity/audit_log.h"

namespace Envoy {
namespace Http {

HttpModSecurityFilterConfig::HttpModSecurityFilterConfig(const envoy::config::filter::http::modsec::v2::Decoder& proto_config,
                                                         Server::Configuration::FactoryContext& context)
    : decoder_(proto_config) {

    modsec_.reset(new modsecurity::ModSecurity());
    modsec_->setConnectorInformation("ModSecurity-test v0.0.1-alpha (ModSecurity test)");
    modsec_->setServerLogCb(HttpModSecurityFilter::_logCb, modsecurity::RuleMessageLogProperty |
                                                           modsecurity::IncludeFullHighlightLogProperty);

    modsec_rules_.reset(new modsecurity::Rules());
    
    for (int i = 0; i < decoder().rules_path_size(); i++ ){
        int rulesLoaded = modsec_rules_->loadFromUri(decoder().rules_path(i).c_str());
        ENVOY_LOG(debug, "Loading ModSecurity config from {}", decoder().rules_path(i));
        if (rulesLoaded == -1) {
            ENVOY_LOG(error, "Failed to load rules: {}", modsec_rules_->getParserError());
        } else {
            ENVOY_LOG(info, "Loaded {} rules", rulesLoaded);
        };
    }

    for (int i = 0; i < decoder().rules_inline_size(); i++ ){
        int rulesLoaded = modsec_rules_->load(decoder().rules_inline(i).c_str());
        ENVOY_LOG(debug, "Loading ModSecurity config from inline");
        if (rulesLoaded == -1) {
            ENVOY_LOG(error, "Failed to load rules: {}", modsec_rules_->getParserError());
        } else {
            ENVOY_LOG(info, "Loaded {} rules", rulesLoaded);
        };
    }

    if (decoder().remotes_overwrite_on_success()) {
        bool hasErrors = false;
        modsecurity::Rules* new_modsec_rules = new modsecurity::Rules();
        for (int i = 0; i < decoder().remotes_size(); i++ ){
            int rulesLoaded = new_modsec_rules->loadRemote(decoder().remotes(i).key().c_str(), decoder().remotes(i).url().c_str());
            ENVOY_LOG(debug, "Loading ModSecurity config from remote url {}", decoder().remotes(i).url());
            if (rulesLoaded == -1) {
                hasErrors = true;
                ENVOY_LOG(error, "Failed to load one remote rules: {}. We fallback to local rules.", new_modsec_rules->getParserError());
                break;
            } else {
                ENVOY_LOG(info, "Loaded {} rules remotely", rulesLoaded);
            };
        }
        if (!hasErrors){
            modsec_rules_.reset(new_modsec_rules);
        }
    }else {
        for (int i = 0; i < decoder().remotes_size(); i++ ){
            int rulesLoaded = modsec_rules_->loadRemote(decoder().remotes(i).key().c_str(), decoder().remotes(i).url().c_str());
            ENVOY_LOG(debug, "Loading ModSecurity config from remote url {}", decoder().remotes(i).url());
            if (rulesLoaded == -1) {
                ENVOY_LOG(error, "Failed to load rules: {}", modsec_rules_->getParserError());
            } else {
                ENVOY_LOG(info, "Loaded {} rules", rulesLoaded);
            };
        }
    }
    
}

HttpModSecurityFilterConfig::~HttpModSecurityFilterConfig() {
}

HttpModSecurityFilter::HttpModSecurityFilter(HttpModSecurityFilterConfigSharedPtr config)
    : config_(config), intervined_(false), request_processed_(false), response_processed_(false), logged_(false), no_audit_log_(false) {
    
    modsec_transaction_.reset(new modsecurity::Transaction(config_->modsec_.get(), config_->modsec_rules_.get(), this));
}

HttpModSecurityFilter::~HttpModSecurityFilter() {
}


void HttpModSecurityFilter::onDestroy() {
    modsec_transaction_->processLogging();
}

const char* getProtocolString(const Protocol protocol) {
    switch (protocol) {
    case Protocol::Http10:
        return "1.0";
    case Protocol::Http11:
        return "1.1";
    case Protocol::Http2:
        return "2.0";
    case Protocol::Http3:
        return "3.0";
    }
  NOT_REACHED_GCOVR_EXCL_LINE;
}

FilterHeadersStatus HttpModSecurityFilter::decodeHeaders(RequestHeaderMap& headers, bool end_stream) {
    ENVOY_LOG(debug, "HttpModSecurityFilter::decodeHeaders");
    if (decoder_callbacks_->route() == nullptr) {
        return Http::FilterHeadersStatus::Continue;
    }
    if (intervined_ || request_processed_) {
        ENVOY_LOG(debug, "Processed");
        return getRequestHeadersStatus();
    }
    // TODO - do we want to support dynamicMetadata?
    const auto& metadata = decoder_callbacks_->route()->routeEntry()->metadata();
    const auto& disable = Envoy::Config::Metadata::metadataValue(&metadata, ModSecurityMetadataFilter::get().ModSecurity, MetadataModSecurityKey::get().Disable);
    ENVOY_LOG(debug, "titi3");
    const auto& disable_request = Envoy::Config::Metadata::metadataValue(&metadata, ModSecurityMetadataFilter::get().ModSecurity, MetadataModSecurityKey::get().DisableRequest);
    const auto& no_audit_log = Envoy::Config::Metadata::metadataValue(&metadata, ModSecurityMetadataFilter::get().ModSecurity, MetadataModSecurityKey::get().NoAuditLog);
     
    if (disable_request.bool_value() || disable.bool_value()) {
        ENVOY_LOG(debug, "Filter disabled");
        request_processed_ = true;
        return FilterHeadersStatus::Continue;
    }
    if (no_audit_log.bool_value()) {
        no_audit_log_ = true;
    }
    auto downstreamAddress = decoder_callbacks_->streamInfo().downstreamLocalAddress();
    // TODO - Upstream is (always?) still not resolved in this stage. Use our local proxy's ip. Is this what we want?
    ASSERT(decoder_callbacks_->connection() != nullptr);
    auto localAddress = decoder_callbacks_->connection()->localAddress();
    // According to documentation, downstreamAddress should never be nullptr
    ASSERT(downstreamAddress != nullptr);
    ASSERT(downstreamAddress->type() == Network::Address::Type::Ip);
    ASSERT(localAddress != nullptr);
    ASSERT(localAddress->type() == Network::Address::Type::Ip);
    modsec_transaction_->processConnection(downstreamAddress->ip()->addressAsString().c_str(), 
                                          downstreamAddress->ip()->port(),
                                          localAddress->ip()->addressAsString().c_str(), 
                                          localAddress->ip()->port());
    if (interventionLog()) {
        return FilterHeadersStatus::StopIteration;
    }
    auto uri = headers.Path();
    auto method = headers.Method();
    modsec_transaction_->processURI(std::string(uri->value().getStringView()).c_str(), 
                                    std::string(method->value().getStringView()).c_str(),
                                    getProtocolString(decoder_callbacks_->streamInfo().protocol().value_or(Protocol::Http11)));
    if (interventionLog()) {
        return FilterHeadersStatus::StopIteration;
    }
    headers.iterate(
            [this](const Http::HeaderEntry& header) -> HeaderMap::Iterate {
                
                std::string k = std::string(header.key().getStringView());
                std::string v = std::string(header.value().getStringView());
                modsec_transaction_->addRequestHeader(k.c_str(), v.c_str());
                // TODO - does this special case makes sense? it doesn't exist on apache/nginx modsecurity bridges.
                // host header is cannonized to :authority even on http older than 2 
                // see https://github.com/envoyproxy/envoy/issues/2209
                if (k == Headers::get().Host.get()) {
                    modsec_transaction_->addRequestHeader(Headers::get().HostLegacy.get().c_str(), v.c_str());
                }
                return HeaderMap::Iterate::Continue;
            });
    modsec_transaction_->processRequestHeaders();
    if (end_stream) {
        request_processed_ = true;
    }
    if (interventionLog()) {
        return FilterHeadersStatus::StopIteration;
    }
    return getRequestHeadersStatus();
}

FilterDataStatus HttpModSecurityFilter::decodeData(Buffer::Instance& data, bool end_stream) {
    ENVOY_LOG(debug, "HttpModSecurityFilter::decodeData");
    if (intervined_ || request_processed_) {
        ENVOY_LOG(debug, "Processed");
        return getRequestStatus();
    }
    for (const Buffer::RawSlice& slice : data.getRawSlices()) {
        size_t requestLen = modsec_transaction_->getRequestBodyLength();
        // If append fails or append reached the limit, test for intervention (in case SecRequestBodyLimitAction is set to Reject)
        // Note, we can't rely solely on the return value of append, when SecRequestBodyLimitAction is set to Reject it returns true and sets the intervention
        if (modsec_transaction_->appendRequestBody(static_cast<unsigned char*>(slice.mem_), slice.len_) == false ||
            (slice.len_ > 0 && requestLen == modsec_transaction_->getRequestBodyLength())) {
            ENVOY_LOG(debug, "HttpModSecurityFilter::decodeData appendRequestBody reached limit");
            if (interventionLog()) {
                return FilterDataStatus::StopIterationNoBuffer;
            }
            // Otherwise set to process request
            end_stream = true;
            break;
        }
    }

    if (end_stream) {
        request_processed_ = true;
        modsec_transaction_->processRequestBody();
    }
    if (interventionLog()) {
        return FilterDataStatus::StopIterationNoBuffer;
    } 
    return getRequestStatus();
}

FilterTrailersStatus HttpModSecurityFilter::decodeTrailers(RequestTrailerMap&) {
  return FilterTrailersStatus::Continue;
}

void HttpModSecurityFilter::setDecoderFilterCallbacks(StreamDecoderFilterCallbacks& callbacks) {
  decoder_callbacks_ = &callbacks;
}


FilterHeadersStatus HttpModSecurityFilter::encodeHeaders(ResponseHeaderMap& headers, bool end_stream) {
    ENVOY_LOG(debug, "HttpModSecurityFilter::encodeHeaders");
    if (decoder_callbacks_->route() == nullptr) {
        return Http::FilterHeadersStatus::Continue;
    }
    if (intervined_ || response_processed_) {
        ENVOY_LOG(debug, "Processed");
        return getResponseHeadersStatus();
    }
    // TODO - do we want to support dynamicMetadata?
    const auto& metadata = encoder_callbacks_->route()->routeEntry()->metadata();
    const auto& disable = Envoy::Config::Metadata::metadataValue(&metadata, ModSecurityMetadataFilter::get().ModSecurity, MetadataModSecurityKey::get().Disable);
    const auto& disable_response = Envoy::Config::Metadata::metadataValue(&metadata, ModSecurityMetadataFilter::get().ModSecurity, MetadataModSecurityKey::get().DisableResponse);
    if (disable.bool_value() || disable_response.bool_value()) {
        ENVOY_LOG(debug, "Filter disabled");
        response_processed_ = true;
        return FilterHeadersStatus::Continue;
    }

    auto status = headers.Status();
    uint64_t code = Utility::getResponseStatus(headers);
    headers.iterate(
            [this](const Http::HeaderEntry& header) -> HeaderMap::Iterate {
                modsec_transaction_->addResponseHeader(
                    std::string(header.key().getStringView()).c_str(),
                    std::string(header.value().getStringView()).c_str()
                );
                return HeaderMap::Iterate::Continue;
            });
    modsec_transaction_->processResponseHeaders(code, 
            getProtocolString(encoder_callbacks_->streamInfo().protocol().value_or(Protocol::Http11)));
        
    if (end_stream) {
        ENVOY_LOG(debug, "HttpModSecurityFilter::encodeHeaders -> end stream");
        response_processed_ = true;
    }
    
    if (interventionLog()) {
        return FilterHeadersStatus::StopIteration;
    }
    return getResponseHeadersStatus();
}

FilterHeadersStatus HttpModSecurityFilter::encode100ContinueHeaders(ResponseHeaderMap& headers) {
    return FilterHeadersStatus::Continue;
}

FilterDataStatus HttpModSecurityFilter::encodeData(Buffer::Instance& data, bool end_stream) {
    ENVOY_LOG(debug, "HttpModSecurityFilter::encodeData");
    if (intervined_ || response_processed_) {
        ENVOY_LOG(debug, "Processed");
        return getResponseStatus();
    }
    
    for (const Buffer::RawSlice& slice : data.getRawSlices()) {
        size_t responseLen = modsec_transaction_->getResponseBodyLength();
        // If append fails or append reached the limit, test for intervention (in case SecResponseBodyLimitAction is set to Reject)
        // Note, we can't rely solely on the return value of append, when SecResponseBodyLimitAction is set to Reject it returns true and sets the intervention
        if (modsec_transaction_->appendResponseBody(static_cast<unsigned char*>(slice.mem_), slice.len_) == false ||
            (slice.len_ > 0 && responseLen == modsec_transaction_->getResponseBodyLength())) {
            ENVOY_LOG(debug, "HttpModSecurityFilter::encodeData appendResponseBody reached limit");
            if (interventionLog()) {
                return FilterDataStatus::StopIterationNoBuffer;
            }
            // Otherwise set to process response
            end_stream = true;
            break;
        }
    }

    if (end_stream) {
        response_processed_ = true;
        modsec_transaction_->processResponseBody();
    }
    if (interventionLog()) {
        return FilterDataStatus::StopIterationNoBuffer;
    }
    return getResponseStatus();
}

FilterTrailersStatus HttpModSecurityFilter::encodeTrailers(ResponseTrailerMap&) {
    return FilterTrailersStatus::Continue;
}


FilterMetadataStatus HttpModSecurityFilter::encodeMetadata(MetadataMap& metadata_map) {
    return FilterMetadataStatus::Continue;
}

void HttpModSecurityFilter::setEncoderFilterCallbacks(StreamEncoderFilterCallbacks& callbacks) {
    encoder_callbacks_ = &callbacks;
}

bool HttpModSecurityFilter::interventionLog() {
    if (intervined_ || (modsec_transaction_->m_it.status == 200 && !modsec_transaction_->m_it.disruptive)) {
        return intervined_;
    }
    if (!logged_ && !no_audit_log_) {
        logged_ = true;
        int parts = modsec_transaction_->m_rules->m_auditLog->getParts();
        if (modsec_transaction_->m_rules->m_auditLog->m_format == modsecurity::audit_log::AuditLog::JSONAuditLogFormat) {
            ENVOY_LOG(warn, "{}", modsec_transaction_->toJSON(parts));
        } else {
            std::string boundary;
            generateBoundary(&boundary);
            ENVOY_LOG(warn, "{}", modsec_transaction_->toOldAuditLogFormat(parts, "-" + boundary + "--"));
        }
        
    }
    if (modsec_transaction_->m_it.disruptive) {
        intervined_ = true;
        ENVOY_LOG(debug, "intervention");
        decoder_callbacks_->sendLocalReply(static_cast<Http::Code>(modsec_transaction_->m_it.status), 
                                           "empty\n",
                                           [](Http::HeaderMap& headers) {
                                           }, absl::nullopt, "");
    }
    return intervined_;
}


FilterHeadersStatus HttpModSecurityFilter::getRequestHeadersStatus() {
    if (intervined_) {
        ENVOY_LOG(debug, "StopIteration");
        return FilterHeadersStatus::StopIteration;
    }
    if (request_processed_) {
        ENVOY_LOG(debug, "Continue");
        return FilterHeadersStatus::Continue;
    }
    // If disruptive, hold until request_processed_, otherwise let the data flow.
    ENVOY_LOG(debug, "RuleEngine");
    return modsec_transaction_->getRuleEngineState() == modsecurity::Rules::EnabledRuleEngine ? 
                FilterHeadersStatus::StopIteration : 
                FilterHeadersStatus::Continue;
}

FilterDataStatus HttpModSecurityFilter::getRequestStatus() {
    if (intervined_) {
        ENVOY_LOG(debug, "StopIterationNoBuffer");
        return FilterDataStatus::StopIterationNoBuffer;
    }
    if (request_processed_) {
        ENVOY_LOG(debug, "Continue");
        return FilterDataStatus::Continue;
    }
    // If disruptive, hold until request_processed_, otherwise let the data flow.
    ENVOY_LOG(debug, "RuleEngine");
    return modsec_transaction_->getRuleEngineState() == modsecurity::Rules::EnabledRuleEngine ? 
                FilterDataStatus::StopIterationAndBuffer : 
                FilterDataStatus::Continue;
}

FilterHeadersStatus HttpModSecurityFilter::getResponseHeadersStatus() {
    if (intervined_ || response_processed_) {
        // If intervined, let encodeData return the localReply
        ENVOY_LOG(debug, "Continue");
        return FilterHeadersStatus::Continue;
    }
    // If disruptive, hold until response_processed_, otherwise let the data flow.
    ENVOY_LOG(debug, "RuleEngine");
    return modsec_transaction_->getRuleEngineState() == modsecurity::Rules::EnabledRuleEngine ? 
                FilterHeadersStatus::StopIteration : 
                FilterHeadersStatus::Continue;
}

FilterDataStatus HttpModSecurityFilter::getResponseStatus() {
    if (intervined_ || response_processed_) {
        // If intervined, let encodeData return the localReply
        ENVOY_LOG(debug, "Continue");
        return FilterDataStatus::Continue;
    }
    // If disruptive, hold until response_processed_, otherwise let the data flow.
    ENVOY_LOG(debug, "RuleEngine");
    return modsec_transaction_->getRuleEngineState() == modsecurity::Rules::EnabledRuleEngine ? 
                FilterDataStatus::StopIterationAndBuffer : 
                FilterDataStatus::Continue;

}

void HttpModSecurityFilter::generateBoundary(std::string *boundary) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    for (int i = 0; i < 8; ++i) {
        boundary->append(1, alphanum[rand() % (sizeof(alphanum) - 1)]);
    }
}

void HttpModSecurityFilter::_logCb(void *data, const void *ruleMessagev) {
    auto filter_ = reinterpret_cast<HttpModSecurityFilter*>(data);
    auto ruleMessage = reinterpret_cast<const modsecurity::RuleMessage *>(ruleMessagev);

    filter_->logCb(ruleMessage);
}

void HttpModSecurityFilter::logCb(const modsecurity::RuleMessage * ruleMessage) {
    if (ruleMessage == nullptr) {
        ENVOY_LOG(error, "ruleMessage == nullptr");
        return;
    }

    ENVOY_LOG(debug, "Rule Id: {} phase: {}",
                    ruleMessage->m_ruleId,
                    ruleMessage->m_phase);
    ENVOY_LOG(debug, "* {} action. {}",
                    // Note - since ModSecurity >= v3.0.3 disruptive actions do not invoke the callback
                    // see https://github.com/SpiderLabs/ModSecurity/commit/91daeee9f6a61b8eda07a3f77fc64bae7c6b7c36
                    ruleMessage->m_isDisruptive ? "Disruptive" : "Non-disruptive",
                    modsecurity::RuleMessage::log(ruleMessage));
    // TODO re-activate webhook
    // config_->webhook_fetcher()->invoke(getRuleMessageAsJsonString(ruleMessage));
}

} // namespace Http
} // namespace Envoy
