/*
 * A stand-in for lightdm-webkit2-greeter's `lightdm' object, for looking at
 * the LikeOS sign-in theme in an ordinary browser (see preview.sh).  Never
 * installed with the theme.
 *
 * Users: alex (no picture, password "likeos"), anna (a picture, password
 * "likeos"), and "ronja" (no picture).  Any other name fails like a wrong
 * password.
 *
 * Without a URL hash the mock answers asynchronously, like the real
 * greeter, for trying the theme by hand.  With a hash it answers
 * synchronously and puts the page into a named state before the page has
 * finished loading, so a headless browser's screenshot shows that state:
 *   #lock          lock view (alex remembered)
 *   #signin        sign-in view, alex remembered
 *   #signin-photo  sign-in view, anna remembered (with a picture)
 *   #other         sign-in view, nobody remembered
 *   #other-typed   sign-in view, "anna" typed into the user name
 *   #error         alex, wrong password, Caps Lock on
 *   #error-other   unknown user typed, wrong password, Caps Lock on
 *   #empty-user    sign-in pressed with no user name
 *   #busy          alex, waiting for the answer
 *   #power         sign-in view with the power menu open
 *   #pam-message   a service question (new password) with a message
 *
 * Copyright (C) 2026 The LikeOS Project
 */
(function () {
	'use strict';

	var scenario = (location.hash || '').replace('#', '');
	var sync = !!scenario;

	var photo = 'data:image/svg+xml;utf8,' + encodeURIComponent(
		'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 96 96">' +
		'<defs><linearGradient id="a" x1="0" y1="0" x2="1" y2="1">' +
		'<stop offset="0" stop-color="#f6b36b"/><stop offset="1" stop-color="#c0477a"/>' +
		'</linearGradient></defs><rect width="96" height="96" fill="url(#a)"/>' +
		'<circle cx="30" cy="34" r="26" fill="#ffe1a8" opacity=".55"/>' +
		'<path d="M0 70 Q30 52 54 66 T96 60 V96 H0Z" fill="#5a2a6e" opacity=".75"/>' +
		'</svg>');

	var users = [
		{ username: 'alex', display_name: 'Alex Weber', real_name: 'Alex Weber', image: '', session: 'likeos', logged_in: false },
		{ username: 'anna', display_name: 'Anna Lindqvist', real_name: 'Anna Lindqvist', image: photo, session: 'likeos', logged_in: false },
		{ username: 'ronja', display_name: 'ronja', real_name: '', image: '', session: '', logged_in: false }
	];
	var passwords = { alex: 'likeos', anna: 'likeos', ronja: 'likeos' };

	var hint = { lock: 'alex', signin: 'alex', 'signin-photo': 'anna', error: 'alex',
		busy: 'alex', power: 'alex', 'pam-message': 'alex' }[scenario] || '';
	if (scenario === 'other' || scenario === 'other-typed' || scenario === 'error-other' ||
	    scenario === 'empty-user')
		hint = '';

	function later(fn, ms) {
		if (sync) fn(); else setTimeout(fn, ms || 350);
	}

	var state = { user: null, inAuth: false, ok: false, stall: scenario === 'busy' };

	window.lightdm = {
		get users() { return users; },
		get num_users() { return users.length; },
		get select_user_hint() { return hint; },
		get default_session() { return 'likeos'; },
		get sessions() { return [{ key: 'likeos', name: 'LikeOS', comment: '' }]; },
		get hostname() { return 'likeos-pc'; },
		get can_shutdown() { return true; },
		get can_restart() { return true; },
		get can_suspend() { return false; },
		get can_hibernate() { return false; },
		get lock_hint() { return false; },
		get layout() { return { name: 'de', short_description: 'de', description: 'German' }; },
		get layouts() { return [this.layout]; },
		get in_authentication() { return state.inAuth; },
		get is_authenticated() { return state.ok; },
		get authentication_user() { return state.user; },
		authenticate: function (user) {
			state.user = user;
			state.inAuth = true;
			state.ok = false;
			state.step = 0;
			later(function () { window.show_prompt('Password: ', 'password'); });
		},
		respond: function (text) {
			if (state.stall) return;
			if (scenario === 'pam-message' && state.step++ === 0) {
				later(function () {
					window.show_prompt('Your password has expired. Choose a new one.', 'info');
					window.show_prompt('New password: ', 'password');
				});
				return;
			}
			later(function () {
				state.inAuth = false;
				state.ok = passwords[state.user] === text;
				window.authentication_complete();
			}, 700);
		},
		cancel_authentication: function () { state.inAuth = false; state.user = null; },
		start_session: function (key) {
			document.title = 'session ' + key + ' started for ' + state.user;
		},
		shutdown: function () { document.title = 'shutdown'; },
		restart: function () { document.title = 'restart'; }
	};

	function key(target, k, caps) {
		target.dispatchEvent(new KeyboardEvent('keydown', {
			key: k, bubbles: true, cancelable: true, modifierCapsLock: !!caps
		}));
	}
	function type(input, text) {
		input.value = text;
		input.dispatchEvent(new Event('input', { bubbles: true }));
	}
	function submit() {
		var form = document.getElementById('card');
		form.dispatchEvent(new Event('submit', { bubbles: true, cancelable: true }));
	}

	document.addEventListener('DOMContentLoaded', function () {
		var pw = document.getElementById('password');
		var un = document.getElementById('username');
		switch (scenario) {
		case '': case 'lock':
			return;
		case 'error':
			key(document, 'Shift');
			type(pw, 'wrong');
			submit();
			key(pw, 'a', true);
			return;
		case 'error-other':
			key(document, 'Shift');
			type(un, 'bob');
			type(pw, 'secret');
			submit();
			key(pw, 'a', true);
			return;
		case 'empty-user':
			key(document, 'Shift');
			submit();
			return;
		case 'other-typed':
			key(document, 'Shift');
			type(un, 'anna');
			pw.focus();
			return;
		case 'busy':
			key(document, 'Shift');
			type(pw, 'likeos');
			submit();
			return;
		case 'pam-message':
			key(document, 'Shift');
			type(pw, 'likeos');
			submit();
			return;
		case 'power':
			key(document, 'Shift');
			document.getElementById('power').click();
			return;
		default:
			key(document, 'Shift');
		}
	});
})();
