package com.hunter.app;

import android.content.Context;
import android.content.Intent;

import androidx.test.core.app.ApplicationProvider;
import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.rule.ActivityTestRule;

import org.junit.Rule;
import org.junit.Test;
import org.junit.runner.RunWith;

import static org.junit.Assert.*;

/**
 * Instrumented tests for MainActivity.
 * Tests UI components and VPN service interaction.
 */
@RunWith(AndroidJUnit4.class)
public class MainActivityTest {

    @Rule
    public ActivityTestRule<MainActivity> activityRule = 
            new ActivityTestRule<>(MainActivity.class);

    @Test
    public void testActivityLaunches() {
        MainActivity activity = activityRule.getActivity();
        assertNotNull("MainActivity should launch", activity);
    }

    @Test
    public void testConnectButtonExists() {
        MainActivity activity = activityRule.getActivity();
        assertNotNull("Connect button should exist", 
                activity.findViewById(R.id.connect_button));
    }

    @Test
    public void testStatusTextExists() {
        MainActivity activity = activityRule.getActivity();
        assertNotNull("Status text should exist", 
                activity.findViewById(R.id.status_text));
    }

    @Test
    public void testStatusDetailExists() {
        MainActivity activity = activityRule.getActivity();
        assertNotNull("Status detail should exist", 
                activity.findViewById(R.id.status_detail));
    }

    @Test
    public void testConfigCountExists() {
        MainActivity activity = activityRule.getActivity();
        assertNotNull("Config count should exist", 
                activity.findViewById(R.id.config_count));
    }

    @Test
    public void testVpnServiceNotActiveByDefault() {
        assertFalse("VPN should not be active by default", 
                VpnState.isActive());
    }

    @Test
    public void useAppContext() {
        Context appContext = ApplicationProvider.getApplicationContext();
        assertEquals("ir.filterbypass.app", appContext.getPackageName());
    }
}
