//
// Created by fang on 2022/7/30.
//

#include "presenter/mine_collection.hpp"
#include "utils/config_helper.hpp"
#include "borealis/core/logger.hpp"
#include "bilibili.h"
#include "borealis/core/i18n.hpp"

using namespace brls::literals;

MineCollectionRequest::MineCollectionRequest() = default;

void MineCollectionRequest::onCollectionList(const bilibili::CollectionListResultWrapper &result) {}

void MineCollectionRequest::onError(const std::string &error) {}

void MineCollectionRequest::requestData(bool refresh) {
    CHECK_REQUEST
    if (refresh) {
        index   = 1;
        hasMore = true;
    }

    if (hasMore) {
        auto mid = ProgramConfig::instance().getUserID();
        if (mid.empty() || mid == "0") {
            this->onError("wiliwili/home/common/no_login"_i18n);
            return;
        }
        this->requestCollectionList(mid, index);
    }
}

void MineCollectionRequest::requestCollectionList(std::string &mid, int i, int num) {
    CHECK_AND_SET_REQUEST
    ASYNC_RETAIN
    BILI::get_my_collection_list(
        mid, i, num, requestType,
        [ASYNC_TOKEN](const bilibili::CollectionListResultWrapper &result) {
            ASYNC_RELEASE
            UNSET_REQUEST
            if (index != result.index) {
                brls::Logger::error(
                    "MineCollectionRequest::requestCollectionList: request: "
                    "{}, got: {}",
                    index, result.index);
                return;
            }
            hasMore = result.has_more;
            index   = result.index + 1;
            this->onCollectionList(result);
        },
        [ASYNC_TOKEN](BILI_ERR) {
            ASYNC_RELEASE
            UNSET_REQUEST
            if (code == -101 || code == -6) {
                this->onError("wiliwili/home/common/no_login"_i18n);
            } else {
                this->onError(error);
            }
        });
}
void MineCollectionRequest::setRequestType(int type) { requestType = type; }

int MineCollectionRequest::getRequestType() { return requestType; }
