/*
 * LikeOS lock and sign-in screen: behaviour.
 *
 * Talks to lightdm-webkit2-greeter's `lightdm' object:
 *   authenticate(user), respond(text), cancel_authentication(),
 *   start_session(key), shutdown(), restart(), and the properties users,
 *   select_user_hint, default_session, sessions, can_shutdown, can_restart,
 *   in_authentication, is_authenticated, hostname, layout.
 * The greeter calls back window.show_prompt(text, type), window.show_message
 * (2.2.5 delivers messages through show_prompt with type "error"/"info"),
 * window.authentication_complete() and window.autologin_timer_expired().
 *
 * The greeter treats any console message containing words such as "error"
 * or "exception" as a broken theme and offers to replace it, so this file
 * never writes to the console and catches everything it calls.
 *
 * Copyright (C) 2026 The LikeOS Project
 */
(function () {
	'use strict';

	var LAST_USER_KEY = 'likeos.greeter.last-user';
	var IDLE_MS = 60000;          /* sign-in view -> lock view */
	var FADE_MS = 480;            /* fade to black before the session */

	var T = {
		signIn: 'Sign in',
		next: 'Continue',
		otherUser: 'Other user',
		backTo: 'Back to ',
		needUser: 'Enter your user name.',
		wrongPassword: 'That password is not correct. Try again.',
		wrongEither: 'Wrong user name or password.',
		noSession: 'The session could not be started.',
		noGreeter: 'The sign-in service is not available.',
		restarting: 'Restarting…',
		shuttingDown: 'Shutting down…'
	};

	/* Avatar colours for users without a picture: pairs of LikeOS blues,
	 * teals and violets, picked by a hash of the user name. */
	var TONES = [
		['#1f4fd1', '#3b86ea'], ['#4338ca', '#7464f0'],
		['#0f5f7d', '#1a96ad'], ['#5b34c4', '#9466f2'],
		['#1e3a8a', '#3b74f0'], ['#0b5296', '#2f93d6']
	];

	var $ = function (id) { return document.getElementById(id); };
	var body = document.body;
	var el = {
		time: $('time-hm'), period: $('time-period'), date: $('date'),
		card: $('card'), avatar: $('avatar'), avatarImg: $('avatar-img'),
		avatarLetter: $('avatar-letter'), name: $('name'),
		subtitle: $('subtitle'), username: $('username'),
		password: $('password'), notes: document.querySelector('.notes'),
		status: $('status'), statusText: $('status-text'), caps: $('caps'),
		submit: $('submit'), submitLabel: document.querySelector('#submit .label'),
		sw: $('switch'), swLabel: $('switch-label'),
		swGlyph: $('switch-glyph'), layout: $('layout'),
		layoutName: $('layout-name'), power: $('power'),
		menu: $('power-menu'), restart: $('act-restart'),
		shutdown: $('act-shutdown')
	};

	var S = {
		view: 'lock',
		mode: 'other',       /* 'known': remembered user; 'other': typed */
		known: null,         /* the remembered user (lightdm user object) */
		authUser: null,      /* the user name being authenticated */
		pending: null,       /* the password waiting for the prompt */
		conversation: false, /* PAM asked something we must show */
		pamMessage: null,
		busy: false,
		idle: 0
	};

	/* `lightdm' is the greeter object as the script the greeter injects
	 * hands it to themes; that script takes it from __LightDMGreeter, which
	 * the greeter's web extension puts on the page and which offers the
	 * same calls -- so the page works from either. */
	function ld() { return window.lightdm || window.__LightDMGreeter || null; }

	function safe(fn) {
		return function () {
			try { return fn.apply(this, arguments); } catch (e) { return undefined; }
		};
	}

	/* --- clock --------------------------------------------------------- */

	/* LikeOS speaks English only: the clock and date are English whatever
	 * locale the browser engine reports, in the same form as the taskbar
	 * clock (24-hour time; "Friday, 27 November"). */
	var locale = 'en';
	var WEEKDAYS = ['Sunday', 'Monday', 'Tuesday', 'Wednesday', 'Thursday',
		'Friday', 'Saturday'];
	var MONTHS = ['January', 'February', 'March', 'April', 'May', 'June',
		'July', 'August', 'September', 'October', 'November', 'December'];

	function pad2(n) { return (n < 10 ? '0' : '') + n; }

	var tick = safe(function () {
		var now = new Date();
		el.time.textContent = pad2(now.getHours()) + ':' + pad2(now.getMinutes());
		el.period.textContent = '';
		el.date.textContent = WEEKDAYS[now.getDay()] + ', ' + now.getDate() +
			' ' + MONTHS[now.getMonth()];
		/* Wake again just after the next minute starts. */
		setTimeout(tick, 60000 - (now.getSeconds() * 1000 + now.getMilliseconds()) + 40);
	});

	/* --- users and avatars --------------------------------------------- */

	function users() {
		try { return ld() && ld().users ? Array.prototype.slice.call(ld().users) : []; }
		catch (e) { return []; }
	}

	function findUser(name) {
		if (!name) return null;
		var list = users();
		for (var i = 0; i < list.length; i++)
			if (list[i] && list[i].username === name) return list[i];
		return null;
	}

	function displayName(u) {
		return (u && (u.display_name || u.real_name || u.username)) || '';
	}

	function hash(s) {
		var h = 0;
		for (var i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) | 0;
		return Math.abs(h);
	}

	function setAvatar(user, typed) {
		var a = el.avatar;
		var image = user && user.image;
		var label = user ? displayName(user) : (typed || '');
		a.classList.remove('is-image', 'is-letter', 'is-mark');
		a.style.background = '';
		if (image) {
			el.avatarImg.onerror = function () { setAvatar(null, label); };
			el.avatarImg.src = image;
			a.classList.add('is-image');
			return;
		}
		el.avatarImg.removeAttribute('src');
		var first = Array.from(label.trim())[0];
		if (!first) {
			a.classList.add('is-mark');
			return;
		}
		var tone = TONES[hash((user && user.username) || label) % TONES.length];
		a.style.background = 'linear-gradient(140deg, ' + tone[0] + ', ' + tone[1] + ')';
		el.avatarLetter.textContent = first.toLocaleUpperCase(locale);
		a.classList.add('is-letter');
	}

	function rememberedUser() {
		var l = ld();
		var hint = null;
		try { hint = l && l.select_user_hint; } catch (e) { hint = null; }
		var u = findUser(hint);
		if (u) return u;
		try { u = findUser(window.localStorage.getItem(LAST_USER_KEY)); } catch (e) { u = null; }
		if (u) return u;
		var list = users();
		return list.length === 1 ? list[0] : null;
	}

	function remember(name) {
		try { window.localStorage.setItem(LAST_USER_KEY, name); } catch (e) { /* none */ }
	}

	function hostname() {
		try { return (ld() && ld().hostname) || ''; } catch (e) { return ''; }
	}

	/* --- sign-in card ---------------------------------------------------- */

	function setKnown(u) {
		S.mode = 'known';
		S.known = u;
		setAvatar(u);
		el.name.textContent = displayName(u);
		el.subtitle.textContent = displayName(u) === u.username ? '' : u.username;
		el.username.hidden = true;
		el.username.value = u.username;
		el.sw.hidden = false;
		el.swLabel.textContent = T.otherUser;
		el.swGlyph.firstElementChild.setAttribute('href', '#g-swap');
	}

	function setOther() {
		S.mode = 'other';
		setAvatar(null, '');
		el.name.textContent = T.signIn;
		el.subtitle.textContent = hostname();
		el.username.hidden = false;
		el.username.value = '';
		if (S.known) {
			el.sw.hidden = false;
			el.swLabel.textContent = T.backTo + displayName(S.known);
			el.swGlyph.firstElementChild.setAttribute('href', '#g-back');
		} else {
			el.sw.hidden = true;
		}
	}

	function updateNotes() {
		el.notes.classList.toggle('shown', !el.status.hidden || !el.caps.hidden);
	}

	/* `field' is the input the message is about ('user' or 'password'). */
	function setStatus(text, kind, field) {
		var bad = !!text && kind !== 'info';
		el.statusText.textContent = text || '';
		el.status.hidden = !text;
		el.status.classList.toggle('info', kind === 'info');
		el.username.classList.toggle('bad', bad && field === 'user');
		el.password.classList.toggle('bad', bad && field !== 'user');
		updateNotes();
	}

	function setCaps(on) {
		if (el.caps.hidden === !on) return;
		el.caps.hidden = !on;
		updateNotes();
	}

	function setBusy(on) {
		S.busy = on;
		body.classList.toggle('busy', on);
		el.username.disabled = on;
		el.password.disabled = on;
		el.submit.disabled = on;
		el.submit.setAttribute('aria-busy', on ? 'true' : 'false');
	}

	function shake() {
		el.card.classList.remove('shake');
		void el.card.offsetWidth;  /* restart the animation */
		el.card.classList.add('shake');
	}

	function focusField() {
		var f = (S.mode === 'other' && !el.username.value.trim()) ? el.username : el.password;
		try { f.focus({ preventScroll: true }); } catch (e) { f.focus(); }
	}

	function resetConversation() {
		S.conversation = false;
		el.password.type = 'password';
		el.password.placeholder = 'Password';
		el.submitLabel.textContent = T.signIn;
	}

	function cancelAuth() {
		S.pending = null;
		S.authUser = null;
		resetConversation();
		try {
			var l = ld();
			if (l && l.in_authentication) l.cancel_authentication();
		} catch (e) { /* none */ }
	}

	/* --- views ------------------------------------------------------------ */

	function showSignin() {
		if (S.view === 'signin') return;
		S.view = 'signin';
		body.classList.remove('view-lock');
		body.classList.add('view-signin');
		focusField();
		poke();
	}

	function showLock() {
		closeMenu(false);
		cancelAuth();
		setBusy(false);
		setStatus('');
		setCaps(false);
		el.password.value = '';
		if (S.known && S.mode !== 'known') setKnown(S.known);
		S.view = 'lock';
		body.classList.remove('view-signin');
		body.classList.add('view-lock');
		try { document.activeElement && document.activeElement.blur(); } catch (e) { /* none */ }
		clearTimeout(S.idle);
	}

	function poke() {
		clearTimeout(S.idle);
		if (S.view === 'signin')
			S.idle = setTimeout(function () {
				if (!S.busy && S.view === 'signin') showLock();
				else poke();
			}, IDLE_MS);
	}

	/* --- authentication ------------------------------------------------ */

	var submit = safe(function (ev) {
		if (ev) ev.preventDefault();
		if (S.busy) return;
		poke();
		var l = ld();
		if (!l) { setStatus(T.noGreeter); shake(); return; }

		if (S.conversation) {
			var answer = el.password.value;
			setStatus('');
			setBusy(true);
			S.conversation = false;
			l.respond(answer);
			return;
		}

		var name = S.mode === 'known' ? S.known.username : el.username.value.trim();
		if (!name) {
			setStatus(T.needUser, 'error', 'user');
			shake();
			el.username.focus();
			return;
		}
		S.pending = el.password.value;
		S.pamMessage = null;
		S.authUser = name;
		setStatus('');
		setBusy(true);
		try { if (l.in_authentication) l.cancel_authentication(); } catch (e) { /* none */ }
		l.authenticate(name);
	});

	function failed(message) {
		setBusy(false);
		resetConversation();
		S.pending = null;
		el.password.value = '';
		setStatus(message);
		shake();
		focusField();
	}

	window.show_prompt = safe(function (text, type) {
		if (type === 'error' || type === 'info') {
			window.show_message(text, type);
			return;
		}
		var l = ld();
		text = String(text || '').trim();
		if (type === 'text' && S.authUser && /login|user/i.test(text)) {
			l.respond(S.authUser);
			return;
		}
		if (type !== 'text' && S.pending !== null) {
			var p = S.pending;
			S.pending = null;
			l.respond(p);
			return;
		}
		/* Something else (a new password, a question): ask the user in the
		 * password field, with the service's own words. */
		S.conversation = true;
		setBusy(false);
		el.password.value = '';
		el.password.type = type === 'text' ? 'text' : 'password';
		el.password.placeholder = text.replace(/:\s*$/, '') || 'Password';
		el.submitLabel.textContent = T.next;
		if (S.pamMessage) setStatus(S.pamMessage.text, S.pamMessage.type);
		showSignin();
		el.password.focus();
	});

	window.show_message = safe(function (text, type) {
		text = String(text || '').trim();
		if (!text) return;
		S.pamMessage = { text: text, type: type === 'info' ? 'info' : 'error' };
		setStatus(S.pamMessage.text, S.pamMessage.type);
	});

	window.authentication_complete = safe(function () {
		var l = ld();
		var ok = false;
		try { ok = !!(l && l.is_authenticated); } catch (e) { ok = false; }
		if (ok) {
			remember(S.authUser || (l.authentication_user || ''));
			startSession();
			return;
		}
		var pam = S.pamMessage;
		var generic = /authenticat|incorrect|failure|failed|denied/i;
		var msg = pam && pam.type === 'error' && !generic.test(pam.text) ? pam.text :
			(S.mode === 'known' ? T.wrongPassword : T.wrongEither);
		S.authUser = null;
		failed(msg);
	});

	window.autologin_timer_expired = safe(function () { /* no timed login */ });

	function sessionKey() {
		var l = ld();
		var keys = [];
		try {
			(l.sessions || []).forEach(function (s) { if (s && s.key) keys.push(s.key); });
		} catch (e) { /* none */ }
		var u = findUser(S.authUser);
		var want = [u && u.session, l.default_session];
		for (var i = 0; i < want.length; i++)
			if (want[i] && (!keys.length || keys.indexOf(want[i]) >= 0)) return want[i];
		return keys[0] || null;
	}

	function startSession() {
		body.classList.add('starting');
		setTimeout(safe(function () {
			var l = ld();
			var key = sessionKey();
			var start = l.start_session || l.start_session_sync || l.login;
			try {
				if (key) start.call(l, key); else start.call(l);
			} catch (e) {
				body.classList.remove('starting');
				failed(T.noSession);
			}
		}), FADE_MS);
	}

	/* --- power menu ------------------------------------------------------ */

	function can(prop) {
		try { return !!(ld() && ld()[prop]); } catch (e) { return false; }
	}

	function setupPower() {
		el.restart.hidden = !can('can_restart');
		el.shutdown.hidden = !can('can_shutdown');
		el.power.hidden = el.restart.hidden && el.shutdown.hidden;
	}

	function menuItems() {
		return [el.restart, el.shutdown].filter(function (b) { return !b.hidden; });
	}

	function openMenu() {
		el.menu.hidden = false;
		el.power.setAttribute('aria-expanded', 'true');
		var items = menuItems();
		if (items.length) items[0].focus();
	}

	function closeMenu(refocus) {
		if (el.menu.hidden) return;
		el.menu.hidden = true;
		el.power.setAttribute('aria-expanded', 'false');
		if (refocus) el.power.focus();
	}

	var powerAction = safe(function (what) {
		closeMenu(false);
		cancelAuth();
		setStatus(what === 'restart' ? T.restarting : T.shuttingDown, 'info');
		body.classList.add('starting');
		var l = ld();
		setTimeout(safe(function () {
			if (what === 'restart') l.restart(); else l.shutdown();
		}), FADE_MS);
		/* Still here after a while: the request was refused. */
		setTimeout(function () {
			body.classList.remove('starting');
			setStatus('');
		}, FADE_MS + 8000);
	});

	/* --- keyboard layout (read-only) --------------------------------- */

	function setupLayout() {
		try {
			var lay = ld() && ld().layout;
			var name = lay && (lay.short_description || lay.name);
			if (!name) return;
			name = String(name).split(/[\s\t(]/)[0].toUpperCase().slice(0, 6);
			if (!name) return;
			el.layoutName.textContent = name;
			el.layout.title = lay.description || name;
			el.layout.hidden = false;
		} catch (e) { /* none */ }
	}

	/* --- input --------------------------------------------------------- */

	var MODIFIERS = /^(Shift|Control|Alt|AltGraph|Meta|OS|Super|Hyper|CapsLock|NumLock|ScrollLock|Fn)$/;

	document.addEventListener('keydown', safe(function (e) {
		body.classList.add('kbd');
		if (e.getModifierState) setCaps(e.getModifierState('CapsLock'));
		if (S.view === 'lock') {
			if (S.busy || body.classList.contains('starting')) return;
			/* Printable keys go on into the field that gets the focus. */
			if (e.key && e.key.length !== 1 && !MODIFIERS.test(e.key)) e.preventDefault();
			if (e.key === ' ') e.preventDefault();
			showSignin();
			return;
		}
		poke();
		if (!el.menu.hidden) {
			var items = menuItems();
			var i = items.indexOf(document.activeElement);
			if (e.key === 'Escape') { e.preventDefault(); closeMenu(true); }
			else if (e.key === 'ArrowDown') { e.preventDefault(); items[(i + 1) % items.length].focus(); }
			else if (e.key === 'ArrowUp') { e.preventDefault(); items[(i - 1 + items.length) % items.length].focus(); }
			else if (e.key === 'Tab') closeMenu(false);
			return;
		}
		if (e.key === 'Escape') {
			e.preventDefault();
			if (S.busy) return;
			if (S.mode === 'other' && S.known) { cancelAuth(); setStatus(''); setKnown(S.known); el.password.value = ''; focusField(); }
			else showLock();
		}
	}), true);

	document.addEventListener('keyup', safe(function (e) {
		if (e.getModifierState) setCaps(e.getModifierState('CapsLock'));
	}), true);

	var wake = safe(function (e) {
		body.classList.remove('kbd');
		if (S.view === 'lock') {
			if (e && e.cancelable && e.type !== 'wheel') e.preventDefault();
			showSignin();
		} else {
			poke();
		}
	});
	document.addEventListener('mousedown', wake, true);
	document.addEventListener('touchstart', wake, { capture: true, passive: false });
	document.addEventListener('wheel', wake, { capture: true, passive: true });
	document.addEventListener('mousemove', safe(function () { if (S.view === 'signin') poke(); }), true);

	/* A click anywhere outside the open menu closes it. */
	document.addEventListener('click', safe(function (e) {
		if (!el.menu.hidden && !el.menu.contains(e.target) && e.target !== el.power &&
		    !el.power.contains(e.target))
			closeMenu(false);
	}));

	el.card.addEventListener('submit', submit);

	el.username.addEventListener('keydown', safe(function (e) {
		if (e.key === 'Enter' && el.username.value.trim() && !el.password.value) {
			e.preventDefault();
			el.password.focus();
		}
	}));

	el.username.addEventListener('input', safe(function () {
		var typed = el.username.value.trim();
		var u = findUser(typed);
		setAvatar(u, typed);
		el.name.textContent = u ? displayName(u) : T.signIn;
		if (!el.status.hidden && !S.busy) setStatus('');
	}));

	el.password.addEventListener('input', safe(function () {
		if (!el.status.hidden && !S.busy && !S.conversation) setStatus('');
	}));

	el.sw.addEventListener('click', safe(function () {
		if (S.busy) return;
		cancelAuth();
		setStatus('');
		el.password.value = '';
		if (S.mode === 'known') setOther(); else setKnown(S.known);
		focusField();
	}));

	el.power.addEventListener('click', safe(function () {
		if (el.menu.hidden) openMenu(); else closeMenu(true);
	}));
	el.restart.addEventListener('click', function () { powerAction('restart'); });
	el.shutdown.addEventListener('click', function () { powerAction('shutdown'); });

	el.card.addEventListener('animationend', function () { el.card.classList.remove('shake'); });

	/* --- start ------------------------------------------------------------ */

	var started = false;
	/* Each part on its own: the power menu must not depend on finding the
	 * remembered user, nor the other way round. */
	var start = function () {
		if (started) return;
		started = true;
		safe(setupPower)();
		safe(setupLayout)();
		safe(function () {
			var u = rememberedUser();
			if (u) setKnown(u); else setOther();
		})();
	};

	tick();
	setOther();
	if (ld()) {
		start();
	} else {
		/* The greeter attaches `lightdm' shortly after the page starts --
		 * later on a busy machine, so the page keeps looking (often at
		 * first, then twice a second) for as long as it is shown. */
		var waited = 0;
		var look = function () {
			if (ld()) { start(); return; }
			waited += 1;
			setTimeout(look, waited < 300 ? 50 : 500);
		};
		setTimeout(look, 50);
	}
})();
