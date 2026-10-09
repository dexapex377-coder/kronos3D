/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package com.kronos3d.app;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.os.Bundle;
import android.widget.Button;
import android.widget.TextView;

/**
 * Entry point: the launcher screen, the way the 3.6 port had one. It opens no
 * native code itself, which is the point -- Blender only starts on the tap.
 *
 * .blend taps still skip this screen: the VIEW/EDIT filters live on
 * BlenderActivity, so a file manager starts Blender directly.
 */
public class LauncherActivity extends Activity {

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    setContentView(R.layout.activity_launcher);

    TextView version = findViewById(R.id.launcher_version);
    version.setText(packageVersion());

    Button launch = findViewById(R.id.launcher_button);
    launch.setOnClickListener(view -> {
      startActivity(new Intent(this, BlenderActivity.class));
      /* finish() so Back from Blender does not land on the launcher and make it
       * look like Blender died: from there the app would appear to need several
       * taps to open. */
      finish();
    });

    /* The attribution and the AI-assisted-code notice. Not folded into the
     * launch path on purpose: it must never be one more tap between the user
     * and the app, but it has to be there. */
    Button credits = findViewById(R.id.launcher_credits);
    credits.setOnClickListener(view -> startActivity(new Intent(this, AboutActivity.class)));
  }

  /* versionName out of the manifest, which package.sh keeps in step with the
   * build; no BuildConfig here because there is no Gradle to generate one. */
  private String packageVersion() {
    try {
      PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
      return info.versionName;
    }
    catch (Exception ex) {
      return "";
    }
  }
}
