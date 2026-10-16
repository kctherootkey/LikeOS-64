/* LikeOS website -- the screenshot and video slideshow.
 *
 * Slides advance only when asked (buttons, thumbnails, arrow keys, a
 * swipe).  The stage goes fullscreen through the Fullscreen API where
 * the browser offers it for an element, and otherwise (iPhone Safari)
 * through a fixed full-window layer; either way the exit button and
 * Escape leave it.
 *
 * Copyright (C) 2026 The LikeOS Project
 */
(function () {
  "use strict";

  var root = document.querySelector("[data-slideshow]");
  var dataEl = document.getElementById("slides-data");
  if (!root || !dataEl) return;
  var slides = JSON.parse(dataEl.textContent);
  if (!slides.length) return;

  var stage = root.querySelector(".stage");
  var media = stage.querySelector(".media");
  var counter = root.querySelector(".counter");
  var fsCaption = stage.querySelector(".fs-caption");
  var capTitle = root.querySelector(".slide-caption h2");
  var capText = root.querySelector(".slide-caption p");
  var capBadges = root.querySelector(".slide-caption .badges");
  var openLink = root.querySelector(".open-original");
  var thumbs = root.querySelector(".thumbs");
  var current = -1;

  var ICON_PLAY = '<svg viewBox="0 0 24 24" fill="currentColor" aria-hidden="true"><path d="M8 5v14l11-7z"/></svg>';

  function esc(s) {
    return String(s).replace(/[&<>"']/g, function (c) {
      return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c];
    });
  }

  /* thumbnails */
  slides.forEach(function (s, i) {
    var b = document.createElement("button");
    b.type = "button";
    b.className = "thumb";
    b.setAttribute("aria-label", "Show slide " + (i + 1) + ": " + s.title);
    b.innerHTML = '<img src="' + esc(s.thumb) + '" alt="" loading="lazy" decoding="async">' +
      (s.type === "video" ? '<span class="play">' + ICON_PLAY + "</span>" : "") +
      "<span>" + esc(s.short || s.title) + "</span>";
    b.addEventListener("click", function () { show(i, true); });
    thumbs.appendChild(b);
  });

  function preload(i) {
    var s = slides[(i + slides.length) % slides.length];
    if (s && s.type === "image") { var im = new Image(); im.src = s.src; }
  }

  function show(i, fromUser) {
    i = (i + slides.length) % slides.length;
    if (i === current) return;
    current = i;
    var s = slides[i];
    stage.classList.toggle("is-video", s.type === "video");
    if (s.type === "video") {
      /* Nothing is fetched from YouTube until the visitor asks for the
       * video: a local poster with a play button stands in for it. */
      /* the note's class must not be "consent": that is the cookie
       * banner's, whose fixed position took the note out of the poster */
      media.innerHTML = '<div class="video-poster" role="button" tabindex="0" aria-label="Play the video: ' + esc(s.title) + '">' +
        '<img src="' + esc(s.poster || s.thumb) + '" alt="">' +
        '<span class="big-play">' + ICON_PLAY + '</span>' +
        '<span class="poster-note">Plays from YouTube (youtube-nocookie.com) when you click.</span></div>';
      var poster = media.querySelector(".video-poster");
      poster.addEventListener("keydown", function (e) {
        if (e.key === "Enter" || e.key === " ") { e.preventDefault(); poster.click(); }
      });
      poster.addEventListener("click", function () {
        media.innerHTML = '<iframe src="https://www.youtube-nocookie.com/embed/' + esc(s.youtube) +
          '?rel=0&amp;playsinline=1&amp;modestbranding=1&amp;autoplay=1" title="' + esc(s.title) + '"' +
          ' allow="accelerometer; autoplay; clipboard-write; encrypted-media; gyroscope; picture-in-picture; web-share; fullscreen"' +
          ' referrerpolicy="strict-origin-when-cross-origin" allowfullscreen></iframe>';
      });
    } else {
      media.innerHTML = '<img class="slide-img" src="' + esc(s.src) + '" alt="' + esc(s.alt || s.title) +
        '" width="1920" height="1200" decoding="async">';
      media.querySelector("img").addEventListener("click", toggleFullscreen);
    }
    counter.textContent = (i + 1) + " / " + slides.length;
    capTitle.textContent = s.title;
    capText.innerHTML = s.text;
    capBadges.innerHTML = (s.badges || []).map(function (b, k) {
      return '<span class="badge' + (k ? " gray" : "") + '">' + esc(b) + "</span>";
    }).join("");
    fsCaption.textContent = s.title;
    if (openLink) {
      openLink.href = s.type === "video" ? "https://youtu.be/" + s.youtube : s.src;
      openLink.querySelector(".label").textContent = s.type === "video" ? "Watch on YouTube" : "Open original";
    }
    var tb = thumbs.children;
    for (var k = 0; k < tb.length; k++) tb[k].setAttribute("aria-current", k === i ? "true" : "false");
    if (tb[i] && fromUser) tb[i].scrollIntoView({ block: "nearest", inline: "center", behavior: "smooth" });
    if (fromUser && history.replaceState) history.replaceState(null, "", "#slide-" + (i + 1));
    preload(i + 1);
    preload(i - 1);
  }

  root.querySelector(".stage-btn.prev").addEventListener("click", function () { show(current - 1, true); });
  root.querySelector(".stage-btn.next").addEventListener("click", function () { show(current + 1, true); });

  /* fullscreen */
  function nativeFs() {
    return document.fullscreenElement || document.webkitFullscreenElement || null;
  }
  function isFs() {
    return !!nativeFs() || stage.classList.contains("pseudo-fs");
  }
  function enterFs() {
    var req = stage.requestFullscreen || stage.webkitRequestFullscreen;
    if (req) {
      try {
        var p = req.call(stage);
        if (p && p.catch) p.catch(function () { pseudo(true); });
        return;
      } catch (e) { /* fall through */ }
    }
    pseudo(true);
  }
  function exitFs() {
    if (nativeFs()) {
      var ex = document.exitFullscreen || document.webkitExitFullscreen;
      if (ex) ex.call(document);
    }
    pseudo(false);
  }
  function pseudo(on) {
    stage.classList.toggle("pseudo-fs", on);
    document.body.classList.toggle("no-scroll", on);
  }
  function toggleFullscreen() { if (isFs()) exitFs(); else enterFs(); }

  root.querySelector(".fs-enter").addEventListener("click", enterFs);
  root.querySelector(".fs-exit").addEventListener("click", exitFs);

  /* keys */
  document.addEventListener("keydown", function (e) {
    var t = e.target;
    if (t && (t.tagName === "INPUT" || t.tagName === "TEXTAREA" || t.isContentEditable)) return;
    if (e.key === "ArrowLeft") { show(current - 1, true); e.preventDefault(); }
    else if (e.key === "ArrowRight") { show(current + 1, true); e.preventDefault(); }
    else if (e.key === "f" || e.key === "F") { toggleFullscreen(); }
    else if (e.key === "Escape" && stage.classList.contains("pseudo-fs")) { pseudo(false); }
  });

  /* swipe */
  var sx = 0, sy = 0, tracking = false;
  stage.addEventListener("touchstart", function (e) {
    if (e.touches.length !== 1) return;
    tracking = true; sx = e.touches[0].clientX; sy = e.touches[0].clientY;
  }, { passive: true });
  stage.addEventListener("touchend", function (e) {
    if (!tracking) return;
    tracking = false;
    var dx = e.changedTouches[0].clientX - sx, dy = e.changedTouches[0].clientY - sy;
    if (Math.abs(dx) > 50 && Math.abs(dx) > Math.abs(dy) * 1.5) show(current + (dx < 0 ? 1 : -1), true);
  }, { passive: true });

  /* start at the slide the address names, if any */
  var m = /^#slide-(\d+)$/.exec(location.hash);
  show(m ? Math.min(Math.max(parseInt(m[1], 10) - 1, 0), slides.length - 1) : 0, false);
})();
