/* LikeOS website -- the cookie consent banner.
 *
 * Every kind of storage starts denied (set in each page's <head>).  This
 * banner asks once; the answer is remembered in this browser and passed on.
 * "Cookie settings" in the footer brings the banner back.
 *
 * Copyright (C) 2026 The LikeOS Project
 */
(function () {
  "use strict";
  var KEY = "likeos-consent";

  function stored() {
    try { return localStorage.getItem(KEY); } catch (e) { return null; }
  }
  function store(v) {
    try { localStorage.setItem(KEY, v); } catch (e) { /* private mode */ }
  }
  function apply(v) {
    var on = v === "granted" ? "granted" : "denied";
    if (typeof window.gtag === "function") {
      window.gtag("consent", "update", {
        ad_storage: on, ad_user_data: on, ad_personalization: on, analytics_storage: on
      });
    }
  }

  var banner;
  function hide() {
    if (banner) { banner.hidden = true; }
  }
  function show() {
    if (!banner) {
      banner = document.createElement("div");
      banner.className = "consent";
      banner.setAttribute("role", "dialog");
      banner.setAttribute("aria-live", "polite");
      banner.setAttribute("aria-label", "Cookie consent");
      banner.innerHTML =
        '<div class="consent-text">' +
        '<strong>Cookies</strong> ' +
        'We use cookies to understand how this site is used and to improve it. ' +
        'They are only set if you agree, and you can change your choice at any time under ' +
        '<em>Cookie settings</em> at the bottom of every page.' +
        '</div>' +
        '<div class="consent-actions">' +
        '<button type="button" class="btn btn-ghost" data-consent="denied">Decline</button>' +
        '<button type="button" class="btn btn-primary" data-consent="granted">Accept</button>' +
        '</div>';
      banner.addEventListener("click", function (e) {
        var b = e.target.closest("[data-consent]");
        if (!b) return;
        var v = b.getAttribute("data-consent");
        store(v);
        apply(v);
        hide();
      });
      document.body.appendChild(banner);
    }
    banner.hidden = false;
  }

  document.addEventListener("click", function (e) {
    if (e.target.closest("[data-consent-open]")) {
      e.preventDefault();
      show();
    }
  });

  if (stored() !== "granted" && stored() !== "denied") show();
})();
