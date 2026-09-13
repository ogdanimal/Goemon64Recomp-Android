package com.goemon64.recomp.touch;

import java.util.EnumMap;
import java.util.Locale;
import java.util.Map;

/**
 * Where every on-screen control sits, how big it is, and whether it is shown.
 *
 * <p>Deliberately free of any {@code android.view} / {@code android.graphics} type.
 * The layout is the part of the overlay with real arithmetic in it — normalisation,
 * scaling, clamping, the C-diamond derivation — so it is kept as plain Java,
 * separate from drawing (see docs/touch-controls.md).
 *
 * <h2>Coordinates</h2>
 * Positions are stored <b>normalised</b> — {@code (0,0)} is the top-left of the
 * usable surface and {@code (1,1)} the bottom-right — so one saved layout is
 * correct on any screen size or aspect ratio. Sizes are <b>not</b> normalised the
 * same way: they are multiples of a single {@code unit}, computed by
 * {@link #sizingUnit}, which is the height of the widest 16:9 box that fits the
 * surface. Sizing against width alone would make every button grow on a 21:9 device,
 * which is the one place there is least room for one; sizing against height alone
 * oversizes them on a squat screen, where the height is nearly the width and the
 * buttons then collide. Taking the smaller of the two is what makes one layout hold
 * from 4:3 to 21:9.
 *
 * <p>The C cluster is stored as a <em>single</em> anchor plus a spread, not four
 * independent positions. The four C buttons are small and always move together on a
 * real pad; giving the editor one handle for the diamond keeps them in formation and
 * makes the cluster possible to place with a thumb.
 */
public final class TouchLayout {

    /** Serialisation version, so a future layout change can migrate rather than reset. */
    public static final int VERSION = 1;

    /** Bounds for the global size multiplier the settings screen offers. */
    public static final float SCALE_MIN = 0.65f;
    public static final float SCALE_MAX = 1.60f;

    /** Bounds for overlay opacity. Never fully opaque: it sits over the game. */
    public static final float OPACITY_MIN = 0.15f;
    public static final float OPACITY_MAX = 0.95f;

    /** When the overlay is shown at all. */
    public enum Visibility {
        /** Show until a gamepad is used, then hide until the screen is touched. */
        AUTO,
        /** Always draw the overlay, even with a controller attached. */
        ALWAYS,
        /** Never draw it — the pre-existing controller-only behaviour. */
        NEVER
    }

    /** A control's placement: normalised centre plus a size multiplier of its own. */
    public static final class Placement {
        public float x;
        public float y;
        public float scale;
        public boolean visible;

        Placement(float x, float y, float scale, boolean visible) {
            this.x = x;
            this.y = y;
            this.scale = scale;
            this.visible = visible;
        }

        Placement copy() {
            return new Placement(x, y, scale, visible);
        }
    }

    private final Map<TouchControl, Placement> placements = new EnumMap<>(TouchControl.class);

    public Visibility visibility = Visibility.AUTO;
    public float globalScale = 1.0f;
    public float opacity = 0.55f;
    public boolean haptics = true;

    /**
     * How far apart the four C buttons sit, as a multiple of one C button's radius.
     * 2.35 leaves a visible gap at the diamond's waist, which is what stops a thumb
     * aimed at C-Up from catching C-Left on the way.
     */
    public float cSpread = 2.35f;

    private TouchLayout() {}

    /**
     * The stock layout, in the arrangement a phone held in two hands wants: stick
     * under the left thumb, A/B under the right thumb with the C diamond just above
     * them, and the three triggers along the top edge under the index fingers.
     *
     * <p>The vertical placement is biased low on purpose. Thumbs rest at the bottom
     * corners, and the top third of a Goemon screen is where the HUD and the horizon
     * live — the two things a hand should never be covering.
     */
    public static TouchLayout defaults() {
        TouchLayout l = new TouchLayout();

        // Left thumb.
        l.placements.put(TouchControl.STICK, new Placement(0.145f, 0.660f, 1.0f, true));

        // Right thumb. B sits up and to the left of A, as on a real N64 pad.
        l.placements.put(TouchControl.A, new Placement(0.900f, 0.730f, 1.0f, true));
        l.placements.put(TouchControl.B, new Placement(0.788f, 0.620f, 1.0f, true));

        // C diamond anchor — one handle, four buttons derived from it.
        l.placements.put(TouchControl.C_UP, new Placement(0.885f, 0.330f, 1.0f, true));
        // The other three C entries carry only scale/visibility; their positions are
        // derived from C_UP's anchor in geometry(). Kept in the map so each can still
        // be hidden individually.
        l.placements.put(TouchControl.C_RIGHT, new Placement(0f, 0f, 1.0f, true));
        l.placements.put(TouchControl.C_DOWN, new Placement(0f, 0f, 1.0f, true));
        l.placements.put(TouchControl.C_LEFT, new Placement(0f, 0f, 1.0f, true));

        // Index fingers, along the top edge.
        l.placements.put(TouchControl.L, new Placement(0.075f, 0.075f, 1.0f, true));
        l.placements.put(TouchControl.Z, new Placement(0.205f, 0.075f, 1.0f, true));
        l.placements.put(TouchControl.R, new Placement(0.925f, 0.075f, 1.0f, true));

        // Centre, out of the way of both thumbs.
        l.placements.put(TouchControl.START, new Placement(0.560f, 0.905f, 1.0f, true));
        l.placements.put(TouchControl.MENU, new Placement(0.440f, 0.905f, 1.0f, true));

        return l;
    }

    public Placement placement(TouchControl control) {
        Placement p = placements.get(control);
        if (p == null) {
            // A control absent from a persisted layout (an older save, a hand-edited
            // file) falls back to its stock placement rather than vanishing.
            p = defaults().placements.get(control).copy();
            placements.put(control, p);
        }
        return p;
    }

    public boolean isVisible(TouchControl control) {
        return placement(control).visible;
    }

    // ----------------------------------------------------------------- geometry

    /** A control resolved to pixels for one particular surface size. */
    public static final class Geometry {
        public final TouchControl control;
        /** Centre, in pixels. */
        public final float cx;
        public final float cy;
        /** Half-width. For round controls this is the radius. */
        public final float rx;
        /** Half-height. Equal to {@link #rx} for everything but SHOULDER. */
        public final float ry;

        Geometry(TouchControl control, float cx, float cy, float rx, float ry) {
            this.control = control;
            this.cx = cx;
            this.cy = cy;
            this.rx = rx;
            this.ry = ry;
        }

        /**
         * Whether a point presses this control.
         *
         * <p>{@code slop} widens the target past what is drawn. Touch targets need to
         * be bigger than they look — a thumb's contact patch is wide and its reported
         * centre sits below where the player thinks they are pressing — but drawing
         * them that big would bury the game. Every emulator front-end that feels good
         * to play does this; the artwork is the label, not the hitbox.
         */
        public boolean hit(float x, float y, float slop) {
            float dx = (x - cx) / (rx * slop);
            float dy = (y - cy) / (ry * slop);
            return dx * dx + dy * dy <= 1.0f;
        }
    }

    /**
     * Base sizes, as multiples of the surface's short edge, before any scaling.
     * A is largest because it is largest on the hardware and is pressed most.
     */
    private static float baseRadius(TouchControl control) {
        switch (control) {
            case STICK:   return 0.150f;
            case A:       return 0.088f;
            case B:       return 0.074f;
            case C_UP: case C_RIGHT: case C_DOWN: case C_LEFT:
                          return 0.052f;
            case L: case Z: case R:
                          return 0.062f;
            case START: case MENU:
                          return 0.042f;
            default:      return 0.060f;
        }
    }

    /**
     * Resolve one control to pixels within a surface of {@code w} x {@code h},
     * inset by the safe-area insets so nothing lands under a display cutout or a
     * gesture bar.
     */
    public Geometry geometry(TouchControl control, float w, float h,
                             float insetLeft, float insetTop,
                             float insetRight, float insetBottom) {
        float availW = Math.max(1f, w - insetLeft - insetRight);
        float availH = Math.max(1f, h - insetTop - insetBottom);
        float unit = sizingUnit(availW, availH);

        Placement p = placement(control);
        float scale = clamp(globalScale, SCALE_MIN, SCALE_MAX) * p.scale;
        float r = baseRadius(control) * unit * scale;

        float cx;
        float cy;
        if (control.isCButton() && control != TouchControl.C_UP) {
            // Derived from the C_UP anchor so the diamond moves as one piece.
            Placement anchor = placement(TouchControl.C_UP);
            float anchorR = baseRadius(TouchControl.C_UP) * unit
                    * clamp(globalScale, SCALE_MIN, SCALE_MAX) * anchor.scale;
            float spread = anchorR * cSpread;
            float ax = insetLeft + anchor.x * availW;
            float ay = insetTop + anchor.y * availH + spread; // diamond centre
            switch (control) {
                case C_RIGHT: cx = ax + spread; cy = ay;          break;
                case C_DOWN:  cx = ax;          cy = ay + spread; break;
                default:      cx = ax - spread; cy = ay;          break; // C_LEFT
            }
        } else {
            cx = insetLeft + p.x * availW;
            cy = insetTop + p.y * availH;
        }

        float rx = r;
        float ry = r;
        if (control.kind == TouchControl.Kind.SHOULDER) {
            // Wider than tall: a trigger is reached with the side of an index finger,
            // which is a long contact patch across the top edge, not a round one.
            rx = r * 1.55f;
            ry = r * 0.72f;
        }

        // Last line of defence: nothing may hang off the usable rect, whatever the
        // aspect ratio and wherever the player dragged it. A control half off the
        // screen is half unpressable, and on the C diamond that would silently cost a
        // whole direction. Clamping can nudge one C member out of formation on an
        // extreme aspect, which is a far smaller problem than losing it off the edge.
        cx = clamp(cx, insetLeft + rx, insetLeft + availW - rx);
        cy = clamp(cy, insetTop + ry, insetTop + availH - ry);

        return new Geometry(control, cx, cy, rx, ry);
    }

    /**
     * The length every control size is a multiple of.
     *
     * <p>The height of the widest 16:9 box that fits: {@code availH} on a 16:9-or-wider
     * screen, and {@code availW * 9/16} on anything squatter. See the class comment for
     * why neither dimension alone works.
     */
    public static float sizingUnit(float availW, float availH) {
        return Math.min(availH, availW * (9f / 16f));
    }

    /** Keep a normalised position inside the surface. */
    public static float clamp01(float v) {
        return clamp(v, 0f, 1f);
    }

    public static float clamp(float v, float lo, float hi) {
        if (Float.isNaN(v)) {
            return lo;
        }
        return v < lo ? lo : (v > hi ? hi : v);
    }

    // -------------------------------------------------------------- persistence

    /**
     * Serialise to a compact single-line form.
     *
     * <p>Hand-rolled rather than JSON because it is stored in SharedPreferences and
     * read back by this class alone; adding org.json here would buy nothing. The
     * parser below ignores anything it does not recognise, so a layout written by a
     * newer build degrades to defaults for the unknown parts instead of throwing.
     */
    public String serialize() {
        StringBuilder sb = new StringBuilder();
        sb.append("v=").append(VERSION);
        sb.append(";vis=").append(visibility.name());
        sb.append(String.format(Locale.US, ";scale=%.4f;op=%.4f;spread=%.4f;hap=%d",
                globalScale, opacity, cSpread, haptics ? 1 : 0));
        for (TouchControl c : TouchControl.values()) {
            Placement p = placement(c);
            sb.append(String.format(Locale.US, ";%s=%.5f,%.5f,%.4f,%d",
                    c.name(), p.x, p.y, p.scale, p.visible ? 1 : 0));
        }
        return sb.toString();
    }

    /** Parse {@link #serialize} output, falling back to defaults for anything bad. */
    public static TouchLayout deserialize(String s) {
        TouchLayout l = defaults();
        if (s == null || s.isEmpty()) {
            return l;
        }
        for (String part : s.split(";")) {
            int eq = part.indexOf('=');
            if (eq <= 0) {
                continue;
            }
            String key = part.substring(0, eq);
            String value = part.substring(eq + 1);
            try {
                switch (key) {
                    case "v":
                        continue;
                    case "vis":
                        l.visibility = Visibility.valueOf(value);
                        continue;
                    case "scale":
                        l.globalScale = clamp(Float.parseFloat(value), SCALE_MIN, SCALE_MAX);
                        continue;
                    case "op":
                        l.opacity = clamp(Float.parseFloat(value), OPACITY_MIN, OPACITY_MAX);
                        continue;
                    case "spread":
                        l.cSpread = clamp(Float.parseFloat(value), 1.6f, 3.5f);
                        continue;
                    case "hap":
                        l.haptics = "1".equals(value);
                        continue;
                    default:
                        break;
                }
                TouchControl control = TouchControl.valueOf(key);
                String[] f = value.split(",");
                if (f.length < 4) {
                    continue;
                }
                Placement p = l.placement(control);
                p.x = clamp01(Float.parseFloat(f[0]));
                p.y = clamp01(Float.parseFloat(f[1]));
                p.scale = clamp(Float.parseFloat(f[2]), 0.5f, 2.0f);
                p.visible = "1".equals(f[3]);
            } catch (IllegalArgumentException ignored) {
                // Unknown enum name or unparseable number: keep the default for that
                // key. A corrupt preference must never stop the overlay from drawing.
            }
        }
        return l;
    }
}
