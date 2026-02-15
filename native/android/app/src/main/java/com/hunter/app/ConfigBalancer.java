package com.hunter.app;

import android.util.Log;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Load balancer for V2Ray configs.
 * Implements round-robin and weighted random selection based on latency.
 */
public class ConfigBalancer {
    private static final String TAG = "ConfigBalancer";
    
    private final List<ConfigManager.ConfigItem> workingConfigs;
    private final AtomicInteger currentIndex;
    private final Random random;
    
    public enum Strategy {
        ROUND_ROBIN,      // Cycle through configs sequentially
        FASTEST_FIRST,    // Always use fastest config
        WEIGHTED_RANDOM   // Random selection weighted by latency (faster = higher chance)
    }
    
    private Strategy strategy = Strategy.WEIGHTED_RANDOM;
    
    public ConfigBalancer() {
        this.workingConfigs = new ArrayList<>();
        this.currentIndex = new AtomicInteger(0);
        this.random = new Random();
    }
    
    /**
     * Update the list of working configs.
     */
    public synchronized void updateConfigs(List<ConfigManager.ConfigItem> configs) {
        workingConfigs.clear();
        
        // Only add configs with valid latency
        for (ConfigManager.ConfigItem config : configs) {
            if (config.latency > 0 && config.latency < 10000) {
                workingConfigs.add(config);
            }
        }
        
        Log.i(TAG, "Updated balancer with " + workingConfigs.size() + " working configs");
    }
    
    /**
     * Get next config based on current strategy.
     */
    public synchronized ConfigManager.ConfigItem getNextConfig() {
        if (workingConfigs.isEmpty()) {
            return null;
        }
        
        switch (strategy) {
            case ROUND_ROBIN:
                return getRoundRobin();
            case FASTEST_FIRST:
                return getFastest();
            case WEIGHTED_RANDOM:
                return getWeightedRandom();
            default:
                return getRoundRobin();
        }
    }
    
    /**
     * Get multiple configs for multi-backend setup.
     */
    public synchronized List<ConfigManager.ConfigItem> getMultipleConfigs(int count) {
        List<ConfigManager.ConfigItem> selected = new ArrayList<>();
        
        if (workingConfigs.isEmpty()) {
            return selected;
        }
        
        int actualCount = Math.min(count, workingConfigs.size());
        
        // Select top N fastest configs
        for (int i = 0; i < actualCount; i++) {
            if (i < workingConfigs.size()) {
                selected.add(workingConfigs.get(i));
            }
        }
        
        return selected;
    }
    
    private ConfigManager.ConfigItem getRoundRobin() {
        int index = currentIndex.getAndIncrement() % workingConfigs.size();
        return workingConfigs.get(index);
    }
    
    private ConfigManager.ConfigItem getFastest() {
        // Configs should already be sorted by latency
        return workingConfigs.get(0);
    }
    
    private ConfigManager.ConfigItem getWeightedRandom() {
        if (workingConfigs.isEmpty()) {
            return null;
        }
        
        // Calculate weights (inverse of latency)
        double[] weights = new double[workingConfigs.size()];
        double totalWeight = 0;
        
        for (int i = 0; i < workingConfigs.size(); i++) {
            ConfigManager.ConfigItem config = workingConfigs.get(i);
            // Weight = 1 / (latency + 100) to avoid division by zero
            // Lower latency = higher weight
            double weight = 1000.0 / (config.latency + 100.0);
            weights[i] = weight;
            totalWeight += weight;
        }
        
        // Select random config based on weights
        double randomValue = random.nextDouble() * totalWeight;
        double cumulativeWeight = 0;
        
        for (int i = 0; i < weights.length; i++) {
            cumulativeWeight += weights[i];
            if (randomValue <= cumulativeWeight) {
                return workingConfigs.get(i);
            }
        }
        
        // Fallback to last config
        return workingConfigs.get(workingConfigs.size() - 1);
    }
    
    public void setStrategy(Strategy strategy) {
        this.strategy = strategy;
        Log.i(TAG, "Strategy changed to: " + strategy);
    }
    
    public Strategy getStrategy() {
        return strategy;
    }
    
    public int getWorkingConfigCount() {
        return workingConfigs.size();
    }
    
    /**
     * Mark a config as failed and remove it from rotation.
     */
    public synchronized void markConfigFailed(String uri) {
        workingConfigs.removeIf(config -> config.uri.equals(uri));
        Log.w(TAG, "Removed failed config. Remaining: " + workingConfigs.size());
    }
}
