#include "ui.h"

#include "ui_internal.h"



#include <lvgl.h>



#include <cstring>



#include "config.h"

#include "fonts.h"

#include "theme.h"

#include "tx_signer.h"

#include "usb_link.h"

#include "wallet.h"



namespace ui {

namespace internal {



namespace {



// Signing scratch lives in BSS — not on the LVGL callback stack (~7 KB).

static txsigner::SignedTx g_signScratch;



void showSignedSuccess(const char* txHash) {

    lv_obj_t* p = panel();

    lv_obj_t* head = heading(p, "SIGNED");

    lv_obj_set_style_text_color(head, theme::Success, 0);

    lv_obj_t* msg = body(p, "Transaction sent to host via USB.");

    lv_obj_set_style_text_color(msg, theme::Silver, 0);

    if (txHash && txHash[0]) {

        char buf[96];

        snprintf(buf, sizeof(buf), "TX hash\n%s", txHash);

        lv_obj_t* hash = fine(p, buf);

        lv_obj_set_style_text_color(hash, theme::Muted, 0);

        lv_obj_set_width(hash, LV_PCT(100));

        lv_label_set_long_mode(hash, LV_LABEL_LONG_WRAP);

    }

    button(p, "BACK", [](lv_event_t*) { showHome(); });

}



void showSigning() {

    lv_obj_t* p = panel();

    lv_obj_t* head = heading(p, "SIGNING");

    lv_obj_set_style_text_color(head, theme::Accent, 0);

    lv_obj_t* msg = body(p, "Hold complete — signing on device…");

    lv_obj_set_style_text_color(msg, theme::Silver, 0);

}



void onSign(lv_event_t*) {

    if (g_signPending) return;

    // Defer crypto to the main loop — running it inside the LVGL long-press

    // callback overflowed the task stack and rebooted the board.

    g_signPending = true;

    showSigning();

}



void showReview(const txsigner::Review& review) {

    lv_obj_t* p = panel();

    const bool ready = review.status == txsigner::Status::Ready;



    char head[40];

    snprintf(head, sizeof(head), "%s  %s", txsigner::statusText(review.status),

             review.type[0] ? review.type : "TX");

    lv_obj_t* st = makeText(p, head);

    theme::applyHeading(st);

    lv_obj_set_style_text_color(st, ready ? theme::Success : theme::Danger, 0);



    if (review.primary[0]) {

        lv_obj_t* prim = body(p, review.primary);

        theme::applyAmount(prim);

        lv_obj_set_style_text_color(prim, theme::Text, 0);

    }

    if (review.secondary[0]) fine(p, review.secondary);



    char meta[192];

    int n = snprintf(meta, sizeof(meta), "%s  -  Fee %s",

                     review.network[0] ? review.network : "MAINNET", review.fee);

    if (n > 0 && n < static_cast<int>(sizeof(meta))) {

        n += snprintf(meta + n, sizeof(meta) - n, "\nSeq %u  -  LastLedger %u",

                      static_cast<unsigned>(review.sequence),

                      static_cast<unsigned>(review.lastLedger));

    }

    if (review.hasSourceTag && n > 0 && n < static_cast<int>(sizeof(meta))) {

        n += snprintf(meta + n, sizeof(meta) - n, "\nSource tag %u",

                      static_cast<unsigned>(review.sourceTag));

    }

    if (review.hasDestinationTag && n > 0 && n < static_cast<int>(sizeof(meta))) {

        n += snprintf(meta + n, sizeof(meta) - n, "\nDest tag %u",

                      static_cast<unsigned>(review.destinationTag));

    }

    if (review.hasFlags && review.flags != 0 && n > 0 && n < static_cast<int>(sizeof(meta))) {

        snprintf(meta + n, sizeof(meta) - n, "\nFlags 0x%08X", static_cast<unsigned>(review.flags));

    }

    lv_obj_t* metaLbl = fine(p, meta);

    lv_obj_set_style_text_color(metaLbl, theme::Muted, 0);

    for (uint8_t i = 0; i < review.fieldCount; ++i) {

        char field[168];

        snprintf(field, sizeof(field), "%s: %s", review.fields[i].name, review.fields[i].value);

        lv_obj_t* fieldLbl = fine(p, field);

        lv_obj_set_style_text_align(fieldLbl, LV_TEXT_ALIGN_LEFT, 0);

        lv_obj_set_style_text_color(fieldLbl, theme::Silver, 0);

    }



    lv_obj_t* warn = makeText(p, review.warning);

    theme::applyBodySm(warn);

    lv_obj_set_style_text_color(warn, ready ? (review.highFee ? theme::Danger : theme::Muted)

                                            : theme::Danger, 0);



    if (ready) {

        lv_obj_t* sign = button(p, review.highFee ? "HOLD - HIGH FEE" : "HOLD TO SIGN", onSign,

                                Btn::Accent, LV_EVENT_LONG_PRESSED);

        lv_obj_set_height(sign, 52);

        if (review.highFee) lv_obj_set_style_bg_color(sign, theme::Danger, 0);

    }

    button(p, "REJECT", [](lv_event_t*) {

        usblink::emitRejected();

        showHome();

    }, Btn::Danger);

}



}  // namespace



void processPendingSign() {

    if (!g_signPending) return;

    g_signPending = false;



    memset(&g_signScratch, 0, sizeof(g_signScratch));

    if (!txsigner::signReviewedPayload(g_pendingPayload, &g_signScratch)) {

        lv_obj_t* p = panel();

        heading(p, "SIGN FAILED");

        lv_obj_t* err = body(p, g_signScratch.error);

        lv_obj_set_style_text_color(err, theme::Danger, 0);

        usblink::emitError(g_signScratch.error);

        button(p, "BACK", [](lv_event_t*) { showHome(); });

        return;

    }

    strncpy(g_signedJson, g_signScratch.json, sizeof(g_signedJson) - 1);

    g_signedJson[sizeof(g_signedJson) - 1] = '\0';

    usblink::emitSigned(g_signedJson);

    showSignedSuccess(g_signScratch.txHash);

}



void submitUnsignedTxImpl(const char* payload) {

    if (!payload || !wallet::isUnlocked()) return;



    g_signPending = false;

    strncpy(g_pendingPayload, payload, sizeof(g_pendingPayload) - 1);

    g_pendingPayload[sizeof(g_pendingPayload) - 1] = '\0';

    static txsigner::Review review;

    txsigner::parseAndReview(g_pendingPayload, &review);

    if (review.status != txsigner::Status::Ready) {

        usblink::emitError(review.warning[0] ? review.warning : "Transaction rejected");

    }

    showReview(review);

}



}  // namespace internal



void submitUnsignedTx(const char* unsignedJson) {

    if (!internal::g_splashDone) return;

    internal::submitUnsignedTxImpl(unsignedJson);

}



}  // namespace ui
