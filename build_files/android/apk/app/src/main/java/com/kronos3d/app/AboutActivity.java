/* SPDX-FileCopyrightText: 2026 Kronos3D Contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

package com.kronos3d.app;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Button;

/**
 * Credits and code quality notice, reachable from the launcher.
 *
 * Attribution is a licence condition here, not a nicety: this is a GPL-3.0
 * fork of Blender, so the credits to the Blender Foundation, to idimus for the
 * original Android bring-up and to Wanderson-Magalhaes for the fork this tree
 * is based on have to travel with the binary. The notice about AI-assisted
 * code sits above them on purpose: it qualifies everything below it.
 */
public class AboutActivity extends Activity {

  @Override
  protected void onCreate(Bundle state) {
    super.onCreate(state);
    setContentView(R.layout.activity_about);

    Button close = findViewById(R.id.about_close);
    close.setOnClickListener(view -> finish());
  }
}
