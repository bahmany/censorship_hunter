import { NextRequest, NextResponse } from 'next/server';
import fs from 'fs';
import path from 'path';

export const dynamic = 'force-dynamic';

const RUNTIME_PATHS = [
  '/app/backend-runtime',
  '/app/runtime',
  './runtime',
];

function findFile(filename: string): string | null {
  for (const dir of RUNTIME_PATHS) {
    const full = path.join(dir, filename);
    try {
      if (fs.existsSync(full) && fs.statSync(full).size > 0) {
        return full;
      }
    } catch {}
  }
  return null;
}

function readGoldConfigs(): string[] {
  const goldPath = findFile('HUNTER_gold.txt');
  if (goldPath) {
    return fs.readFileSync(goldPath, 'utf-8').split('\n').map(l => l.trim()).filter(l => l && !l.startsWith('#'));
  }
  return [];
}

function readAllConfigs(): string[] {
  const tsvPath = findFile('HUNTER_config_db.tsv');
  if (!tsvPath) return [];
  const results: string[] = [];
  for (const line of fs.readFileSync(tsvPath, 'utf-8').split('\n')) {
    if (!line.trim() || line.startsWith('#')) continue;
    const uri = line.split('\t')[0]?.trim();
    if (uri) results.push(uri);
  }
  return results;
}

function readSilverConfigs(): string[] {
  const tsvPath = findFile('HUNTER_config_db.tsv');
  if (!tsvPath) return [];
  const results: string[] = [];
  for (const line of fs.readFileSync(tsvPath, 'utf-8').split('\n')) {
    if (!line.trim() || line.startsWith('#')) continue;
    const parts = line.split('\t');
    if (parts.length >= 7 && parseInt(parts[6], 10) === 0) {
      const uri = parts[0]?.trim();
      if (uri) results.push(uri);
    }
  }
  return results;
}

function getGeneratedConfigs(): string[] {
  const host = 'api.abharcable.com';
  const portBase = 3101;
  const configs: string[] = [];
  for (let i = 1; i <= 20; i++) {
    configs.push(`socks5://${host}:${portBase + i - 1}#abhar-${String(i).padStart(2, '0')}`);
  }
  return configs;
}

export async function GET(request: NextRequest) {
  const gold = readGoldConfigs();
  const silver = readSilverConfigs();
  const all = readAllConfigs();
  const generated = getGeneratedConfigs();

  const meta = {
    generation_timestamp: new Date().toISOString(),
    epoch_timestamp: Math.floor(Date.now() / 1000),
    active_routes_count: gold.length,
    upstream_count: 2,
    backend_count: all.length,
    generated_count: generated.length,
    generated_active: generated.length,
  };

  return NextResponse.json({
    ...meta,
    gold_configs: gold,
    silver_configs: silver,
    generated_configs: generated,
    all_count: all.length,
  }, { headers: { 'Cache-Control': 'no-store' } });
}
