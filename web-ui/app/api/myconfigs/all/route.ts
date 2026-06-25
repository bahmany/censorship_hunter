import { NextRequest, NextResponse } from 'next/server';
import fs from 'fs';
import path from 'path';

export const dynamic = 'force-dynamic';

const RUNTIME_PATHS = ['/app/backend-runtime', '/app/runtime', './runtime'];

function findFile(filename: string): string | null {
  for (const dir of RUNTIME_PATHS) {
    const full = path.join(dir, filename);
    try { if (fs.existsSync(full) && fs.statSync(full).size > 0) return full; } catch {}
  }
  return null;
}

function getMeta() {
  const goldPath = findFile('HUNTER_gold.txt');
  const goldCount = goldPath ? fs.readFileSync(goldPath, 'utf-8').split('\n').filter(l => l.trim() && !l.startsWith('#')).length : 0;
  const tsvPath = findFile('HUNTER_config_db.tsv');
  const allCount = tsvPath ? fs.readFileSync(tsvPath, 'utf-8').split('\n').filter(l => l.trim() && !l.startsWith('#')).length : 0;
  return {
    generation_timestamp: new Date().toISOString(),
    active_routes_count: goldCount,
    upstream_count: 2,
    backend_count: allCount,
  };
}

export async function GET(request: NextRequest) {
  const tsvPath = findFile('HUNTER_config_db.tsv');
  let configs: string[] = [];
  if (tsvPath) {
    for (const line of fs.readFileSync(tsvPath, 'utf-8').split('\n')) {
      if (!line.trim() || line.startsWith('#')) continue;
      const uri = line.split('\t')[0]?.trim();
      if (uri) configs.push(uri);
    }
  }
  const meta = getMeta();

  if (configs.length === 0) {
    return NextResponse.json({ error: 'No configs found', ...meta }, { status: 404, headers: { 'Cache-Control': 'no-store' } });
  }

  const content = configs.join('\n') + '\n';
  return new NextResponse(content, {
    headers: {
      'Content-Type': 'text/plain; charset=utf-8',
      'Content-Disposition': 'attachment; filename="hunter_all_configs.txt"',
      'X-Config-Count': String(configs.length),
      'X-Generation-Timestamp': meta.generation_timestamp,
      'X-Active-Routes': String(meta.active_routes_count),
      'X-Upstream-Count': String(meta.upstream_count),
      'X-Backend-Count': String(meta.backend_count),
      'Cache-Control': 'no-store',
    },
  });
}
