/* LikeOS website -- the navigation drawer on narrow screens.
 *
 * Copyright (C) 2026 The LikeOS Project
 */
(function () {
  "use strict";
  var body = document.body;
  var btn = document.querySelector(".menu-btn");
  var backdrop = document.querySelector(".backdrop");
  var sidebar = document.getElementById("sidebar");
  if (!btn || !sidebar) return;

  function setOpen(open) {
    body.classList.toggle("nav-open", open);
    btn.setAttribute("aria-expanded", open ? "true" : "false");
    if (open) {
      var first = sidebar.querySelector("a");
      if (first) first.focus({ preventScroll: true });
    }
  }

  btn.addEventListener("click", function () {
    setOpen(!body.classList.contains("nav-open"));
  });
  if (backdrop) backdrop.addEventListener("click", function () { setOpen(false); });
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape" && body.classList.contains("nav-open")) {
      setOpen(false);
      btn.focus();
    }
  });
  sidebar.addEventListener("click", function (e) {
    if (e.target.closest("a")) setOpen(false);
  });
  window.addEventListener("resize", function () {
    if (window.innerWidth > 960 && body.classList.contains("nav-open")) setOpen(false);
  });
})();
