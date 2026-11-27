//
// Created by 贾海峰 on 2023/7/7.
//

#include "bilibili.h"
#include "presenter/mine_later.hpp"
#include "borealis/core/i18n.hpp"
#include "utils/config_helper.hpp"

using namespace brls::literals;

void MineLaterRequest::onWatchLaterList(const bilibili::WatchLaterListWrapper& result) {}

void MineLaterRequest::onError(const std::string& error) {}

void MineLaterRequest::requestData(bool refresh) {
    CHECK_REQUEST
    this->requestWatchLaterList();
}

void MineLaterRequest::requestWatchLaterList() {
    auto mid = ProgramConfig::instance().getUserID();
    if (mid.empty() || mid == "0") {
        this->onError("wiliwili/home/common/no_login"_i18n);
        return;
    }
    CHECK_AND_SET_REQUEST
    ASYNC_RETAIN
    BILI::getWatchLater(
        [ASYNC_TOKEN](const auto& result) {
            ASYNC_RELEASE
            UNSET_REQUEST
            this->onWatchLaterList(result);
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