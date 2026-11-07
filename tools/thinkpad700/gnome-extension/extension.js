const ByteArray = imports.byteArray;
const { Gio, GLib, GObject, Shell, St } = imports.gi;
const Main = imports.ui.main;
const PanelMenu = imports.ui.panelMenu;
const PopupMenu = imports.ui.popupMenu;

const UUID = 'thinkpad700c-display-modes@codex.local';
const CONFIG_DIR = GLib.build_filenamev([
    GLib.get_home_dir(),
    '.config',
    'thinkpad700c-display',
]);
const CONFIG_PATH = GLib.build_filenamev([CONFIG_DIR, 'display-mode.json']);
const EFFECT_PREFIX = 'thinkpad700c-display-mode-';

const MODES = [
    ['color', '彩色模式'],
    ['high-contrast-black', '高对比度黑'],
    ['high-contrast-white', '高对比度白（黑白反色）'],
];

var MonochromeEffect = GObject.registerClass(
{ GTypeName: 'ThinkpadMonochromeEffectV1' },
class MonochromeEffect extends Shell.GLSLEffect {
    _init(inverted = false) {
        super._init();

        const location = this.get_uniform_location('invert_amount');
        this.set_uniform_float(
            location, 1, 1, [inverted ? 1.0 : 0.0]);
    }

    vfunc_build_pipeline() {
        /*
         * Use one shader pass for grayscale, inversion and contrast. Stacking
         * Shell.InvertLightnessEffect with two Clutter offscreen effects can
         * leave color channels behind on the RK3588 graphics stack, producing
         * a purple cast instead of a monochrome inverse.
         *
         * Clutter textures are premultiplied-alpha. Convert to straight RGB
         * for luminance calculation and premultiply the monochrome result
         * again to keep translucent Shell elements free of bright fringes.
         */
        const shader = [
            'cogl_texel = texture2D(cogl_sampler, cogl_tex_coord.st);',
            'float alpha = cogl_texel.a;',
            'vec3 rgb = alpha > 0.0001 ? cogl_texel.rgb / alpha : vec3(0.0);',
            'float gray = dot(rgb, vec3(0.2126, 0.7152, 0.0722));',
            'float mapped = mix(gray, 1.0 - gray, invert_amount);',
            'mapped = clamp((mapped - 0.5) * 1.55 + 0.5, 0.0, 1.0);',
            'cogl_texel = vec4(vec3(mapped * alpha), alpha);',
        ].join('\\n');

        this.add_glsl_snippet(
            Shell.SnippetHook.TEXTURE_LOOKUP,
            'uniform float invert_amount;',
            shader,
            true
        );
    }
});

function readMode() {
    if (!GLib.file_test(CONFIG_PATH, GLib.FileTest.EXISTS))
        return 'color';

    try {
        const [ok, contents] = GLib.file_get_contents(CONFIG_PATH);
        if (!ok)
            return 'color';
        const mode = JSON.parse(ByteArray.toString(contents)).mode;
        return MODES.some(([id]) => id === mode) ? mode : 'color';
    } catch (error) {
        logError(error, `Display modes could not read ${CONFIG_PATH}`);
        return 'color';
    }
}
function saveMode(mode) {
    GLib.mkdir_with_parents(CONFIG_DIR, 0o700);
    GLib.file_set_contents(CONFIG_PATH, `${JSON.stringify({ mode }, null, 2)}\n`);
}

function effectActors() {
    const actors = [global.window_group, Main.uiGroup];
    if (global.top_window_group)
        actors.push(global.top_window_group);
    return actors.filter((actor, index) => actor && actors.indexOf(actor) === index);
}

function removeOurEffects(actor) {
    for (const suffix of ['monochrome', 'invert', 'contrast', 'desaturate']) {
        const effect = actor.get_effect(`${EFFECT_PREFIX}${suffix}`);
        if (effect)
            actor.remove_effect(effect);
    }
}

function applyEffects(mode) {
    // Effects alter only the final pixels. Actor geometry and input coordinates
    // are left untouched so the calibrated bezel-safe viewport stays stable.
    for (const actor of effectActors()) {
        removeOurEffects(actor);
        if (mode === 'color')
            continue;

        actor.add_effect_with_name(
            `${EFFECT_PREFIX}monochrome`,
            new MonochromeEffect(mode === 'high-contrast-white')
        );
    }
}

var DisplayModeIndicator = GObject.registerClass(
{ GTypeName: 'ThinkpadSwitchDisplayModeIndicatorV1' },
class DisplayModeIndicator extends PanelMenu.Button {
    _init() {
        super._init(0.0, '显示模式');

        this._icon = new St.Icon({
            icon_name: 'video-display-symbolic',
            style_class: 'system-status-icon',
        });
        this.add_child(this._icon);

        this._items = new Map();
        for (const [mode, label] of MODES) {
            const item = new PopupMenu.PopupMenuItem(label);
            item.connect('activate', () => this.setMode(mode));
            this.menu.addMenuItem(item);
            this._items.set(mode, item);
        }

        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._statusItem = new PopupMenu.PopupMenuItem('', {
            reactive: false,
            can_focus: false,
        });
        this.menu.addMenuItem(this._statusItem);

        this.setMode(readMode(), false);
    }

    setMode(mode, persist = true) {
        if (!MODES.some(([id]) => id === mode))
            mode = 'color';

        this._mode = mode;
        applyEffects(mode);
        if (persist)
            saveMode(mode);

        for (const [id, item] of this._items)
            item.setOrnament(id === mode ? PopupMenu.Ornament.DOT : PopupMenu.Ornament.NONE);

        const label = MODES.find(([id]) => id === mode)[1];
        this._statusItem.label.text = `当前模式：${label}`;
        this._icon.icon_name = mode === 'color'
            ? 'video-display-symbolic'
            : 'preferences-desktop-accessibility-symbolic';
    }

    destroy() {
        applyEffects('color');
        super.destroy();
    }
});

let indicator = null;
let switchPoll = 0;
let lastSwitchKey = null;
const SWITCH_STATUS_PATH = '/run/thinkpad700c-leds/switch-status.json';
const EMULATOR_STATUS_PATH = GLib.build_filenamev([
    GLib.get_user_runtime_dir(),
    'thinkpad700c-emulator',
    'panel-status.json',
]);
const ACK_DIR = GLib.build_filenamev([GLib.get_user_runtime_dir(), 'thinkpad700c-display']);
const ACK_PATH = GLib.build_filenamev([ACK_DIR, 'switch-display-status.json']);
let emulatorOwnsControls = false;

function emulatorIsActive() {
    if (!GLib.file_test(EMULATOR_STATUS_PATH, GLib.FileTest.EXISTS))
        return false;
    try {
        const [ok, bytes] = GLib.file_get_contents(EMULATOR_STATUS_PATH);
        if (!ok)
            return false;
        const state = JSON.parse(ByteArray.toString(bytes));
        const age = Date.now() / 1000 - state.timestamp_unix;
        return Number.isFinite(age) && age >= -5 && age <= 0.75 &&
            state.running === true && state.machine === 'ibmtp700' &&
            state.mouse_captured === true;
    } catch (error) {
        return false;
    }
}

function pollHardwareSwitch() {
    const target = Main.panel.statusArea['thinkpad700c-display-modes'] || indicator;
    if (emulatorIsActive()) {
        if (!emulatorOwnsControls) {
            emulatorOwnsControls = true;
            lastSwitchKey = null;
            // 86Box applies the physical inversion switch to its own texture.
            // Remove the desktop-wide copy so the effect is never doubled.
            if (target && typeof target.setMode === 'function')
                target.setMode('color', false);
        }
        return GLib.SOURCE_CONTINUE;
    }
    if (emulatorOwnsControls) {
        emulatorOwnsControls = false;
        lastSwitchKey = null;
    }
    if (!GLib.file_test(SWITCH_STATUS_PATH, GLib.FileTest.EXISTS)) {
        lastSwitchKey = null;
        return GLib.SOURCE_CONTINUE;
    }
    try {
        const [ok, bytes] = GLib.file_get_contents(SWITCH_STATUS_PATH);
        if (!ok)
            return GLib.SOURCE_CONTINUE;
        const state = JSON.parse(ByteArray.toString(bytes));
        const age = Date.now() / 1000 - state.timestamp_unix;
        if (!Number.isFinite(age) || !state.connected || !state.inputs || age < -5 || age > 3) {
            lastSwitchKey = null;
            return GLib.SOURCE_CONTINUE;
        }
        const invert = state.inputs.invert;
        if ((invert !== 0 && invert !== 1) || typeof state.epoch !== 'string')
            return GLib.SOURCE_CONTINUE;
        const key = `${state.epoch}:${invert}`;
        if (!target || typeof target.setMode !== 'function' || key === lastSwitchKey)
            return GLib.SOURCE_CONTINUE;
        const mode = invert ? 'high-contrast-white' : 'color';
        target.setMode(mode);
        const verified = effectActors().every(actor =>
            invert
                ? !!actor.get_effect(`${EFFECT_PREFIX}monochrome`)
                : !['monochrome', 'invert', 'contrast', 'desaturate'].some(
                    suffix => !!actor.get_effect(`${EFFECT_PREFIX}${suffix}`)));
        GLib.mkdir_with_parents(ACK_DIR, 0o700);
        GLib.file_set_contents(ACK_PATH, JSON.stringify({
            timestamp_unix: Date.now() / 1000, epoch: state.epoch,
            invert, mode, effects_verified: verified,
        }));
        if (verified)
            lastSwitchKey = key;
    } catch (error) {
        logError(error, 'Could not apply ESP32 display switch');
    }
    return GLib.SOURCE_CONTINUE;
}

function enable() {
    // Reuse the active viewport extension's menu and geometry-safe effects.
    if (Main.panel.statusArea['thinkpad700c-display-modes']) {
        lastSwitchKey = null;
        switchPoll = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 200, pollHardwareSwitch);
        pollHardwareSwitch();
        return;
    }
    // Ensure the geometry-changing magnifier path from earlier experiments is off.
    const applications = new Gio.Settings({
        schema_id: 'org.gnome.desktop.a11y.applications',
    });
    applications.set_boolean('screen-magnifier-enabled', false);

    indicator = new DisplayModeIndicator();
    Main.panel.addToStatusArea(UUID, indicator, 1, 'right');
    lastSwitchKey = null;
    switchPoll = GLib.timeout_add(GLib.PRIORITY_DEFAULT, 200, pollHardwareSwitch);
    pollHardwareSwitch();
}

function disable() {
    if (switchPoll) {
        GLib.source_remove(switchPoll);
        switchPoll = 0;
    }
    lastSwitchKey = null;
    emulatorOwnsControls = false;
    if (indicator) {
        indicator.destroy();
        indicator = null;
    }
}
