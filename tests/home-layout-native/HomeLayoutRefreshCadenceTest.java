/* SPDX-License-Identifier: AGPL-3.0-or-later */
package com.sevtinge.hyperceiler.libhook.rules.home.dock;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.SystemClock;
import com.sevtinge.hyperceiler.libhook.provider.HomeLayoutPrefsSnapshot;
import java.util.Map;
import java.util.function.BooleanSupplier;

/** Executes the production worker against the host's observer and Provider transport fixtures. */
public final class HomeLayoutRefreshCadenceTest {
    private static final String PREF = "content://com.sevtinge.hyperceiler.provider.sharedprefs/pref/";
    private static final String MARGIN = "prefs_key_home_layout_workspace_padding_top";
    private static final String PACKAGE = "package:com.sevtinge.hyperceiler";
    private static final int HAPTIC = (57 << 4) | 9;

    private static void check(boolean value, String reason) {
        if (!value) throw new AssertionError(reason);
    }

    private static void await(BooleanSupplier ready, long ms, String reason) throws InterruptedException {
        final long deadline = System.nanoTime() + ms * 1_000_000L;
        while (!ready.getAsBoolean() && System.nanoTime() < deadline) Thread.sleep(10);
        check(ready.getAsBoolean(), reason);
    }

    public static void main(String[] args) throws Exception {
        ContentResolver.setSpecs(HomeLayoutPrefsSnapshot.specs());
        ContentResolver.setRows(Map.of(
            "prefs_key_allow_hook", 1,
            MARGIN + "_enable", 1,
            MARGIN, 60,
            HomeLayoutPrefsSnapshot.HAPTIC_PROFILE_V1, HAPTIC));
        HomeLayoutNativeEndpointOS4.startCombinedPrefetch();
        await(() -> HomeLayoutNativeEndpointOS4.recentsHapticProfileWord() == HAPTIC, 2000, "initial revision");
        check(ContentResolver.queryCount() == 1, "startup must read one complete revision");
        check(ContentResolver.registrationCount() == 1, "observer not registered");
        var initial = HomeLayoutNativeEndpointOS4.readCombinedSnapshot();
        check(initial.available() == 1 && initial.flags() != 0, "initial authorization missing");

        Thread.sleep(1800); // More than the old idle polling interval.
        SystemClock.source = () -> System.nanoTime() + 6_000_000_000L;
        var renewed = HomeLayoutNativeEndpointOS4.readCombinedSnapshot();
        check(renewed.revision() == initial.revision() && renewed.deadline() > initial.deadline()
            && renewed.available() == 1 && renewed.flags() == initial.flags(), "idle authorization expired");
        check(HomeLayoutNativeEndpointOS4.recentsHapticProfileWord() == HAPTIC
            && ContentResolver.queryCount() == 1, "idle lease or vibration woke the Provider");
        SystemClock.source = System::nanoTime;

        ContentResolver.signalChange(PREF + "integer/prefs_key_unrelated_setting");
        Thread.sleep(200);
        check(ContentResolver.queryCount() == 1, "unrelated setting woke the layout reader");

        ContentResolver.putRow(MARGIN, 90);
        ContentResolver.putRow(HomeLayoutPrefsSnapshot.HAPTIC_PROFILE_V1, HAPTIC + 16);
        for (int index = 0; index < 20; ++index)
            ContentResolver.signalChange(PREF + "integer/prefs_key_home_recent_card_haptic_strength");
        await(() -> HomeLayoutNativeEndpointOS4.recentsHapticProfileWord() == HAPTIC + 16, 2000,
            "vibration settings did not publish");
        check(HomeLayoutNativeEndpointOS4.readPreferences().knobDeltaDp()[2] == 60,
            "layout and vibration did not publish together");
        check(ContentResolver.queryCount() <= 3, "change burst was not coalesced");

        ContentResolver.failAt(0, "null");
        Context.signalPackage(new Intent(Intent.ACTION_PACKAGE_REPLACED, Uri.parse(PACKAGE)));
        check(HomeLayoutNativeEndpointOS4.recentsHapticProfileWord() == -1
            && HomeLayoutNativeEndpointOS4.readCombinedSnapshot().available() == 0,
            "package replacement left the authorization live");
        check(HomeLayoutNativeEndpointOS4.readPreferences().knobDeltaDp()[2] == 60,
            "package replacement discarded the last good layout");
        await(() -> ContentResolver.queryCount() >= 4, 13_000, "failed read recovery did not run");
        Thread.sleep(1800);
        check(ContentResolver.queryCount() == 4, "failed Provider was repeatedly started");

        ContentResolver.setRows(Map.of());
        Context.signalPackage(new Intent(Intent.ACTION_PACKAGE_DATA_CLEARED, Uri.parse(PACKAGE)));
        check(HomeLayoutNativeEndpointOS4.readPreferences().knobDeltaDp()[2] == 0,
            "data clear kept stale layout");
        await(() -> ContentResolver.queryCount() == 1, 2000, "package data clear did not refresh");
        check(HomeLayoutNativeEndpointOS4.pauseForHotReload(), "worker failed to retire");
        check(ContentResolver.unregistrationCount() == 1, "observer leaked after retirement");
        ContentResolver.signalChange(PREF + "integer/" + MARGIN);
        Thread.sleep(200);
        check(ContentResolver.queryCount() == 1, "retired endpoint still queried Provider");
        System.out.println("PASS idle configuration, short leases, haptic changes, bounded retries, data clear and retirement");
    }
}
