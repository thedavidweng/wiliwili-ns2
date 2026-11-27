//
// Created by fang on 2022/7/28.
//

#include "bilibili.h"
#include "presenter/mine_history.hpp"
#include "borealis/core/i18n.hpp"
#include "utils/config_helper.hpp"

using namespace brls::literals;

void MineHistoryRequest::onHistoryList(const bilibili::HistoryVideoResultWrapper &result) {}

void MineHistoryRequest::onError(const std::string &error) {}

void MineHistoryRequest::requestData(bool refresh) {
    CHECK_REQUEST
    if (refresh) this->cursor = bilibili::HistoryVideoListCursor();
    this->requestHistoryVideoList();
}

void MineHistoryRequest::requestHistoryVideoList() {
    std::string mid = ProgramConfig::instance().getUserID();
    if (mid.empty() || mid == "0") {
        this->onError("wiliwili/home/common/no_login"_i18n);
        return;
    }
    CHECK_AND_SET_REQUEST
    ASYNC_RETAIN
    BILI::get_my_history(
        cursor,
        [ASYNC_TOKEN](const bilibili::HistoryVideoResultWrapper &result) {
            ASYNC_RELEASE
            UNSET_REQUEST
            this->cursor = result.cursor;
            this->onHistoryList(result);
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