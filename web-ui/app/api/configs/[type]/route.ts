import { NextRequest, NextResponse } from 'next/server'
import { readFile, access, readdir } from 'fs/promises'
import { join } from 'path'

// TSV V2 format (from ConfigDatabase::saveToDisk):
// uri  tag  engine  first_seen  last_tested  last_alive  alive(0/1)  telegram_only(0/1)  latency  fails  tests  passes

interface ParsedConfig {
  uri: string
  alive: boolean
  telegram_only: boolean
  latency: number
  engine: string
  tag: string
  total_tests: number
  total_passes: number
}

function parseTsvLine(line: string): ParsedConfig | null {
  if (!line || line.startsWith('#')) return null
  const parts = line.split('\t')
  if (parts.length < 7) return null
  return {
    uri: parts[0],
    tag: parts[1] || '',
    engine: parts[2] || '',
    alive: parts[6] === '1',
    telegram_only: parts[7] === '1',
    latency: parseFloat(parts[8]) || 0,
    total_tests: parseInt(parts[10]) || 0,
    total_passes: parseInt(parts[11]) || 0,
  }
}

async function findConfigDb(): Promise<string | null> {
  const candidates = [
    '/app/backend-runtime/HUNTER_config_db.tsv',
    '/app/runtime/HUNTER_config_db.tsv',
    join(process.cwd(), 'runtime', 'HUNTER_config_db.tsv'),
    join(process.cwd(), 'backend-runtime', 'HUNTER_config_db.tsv'),
  ]
  for (const p of candidates) {
    try {
      await access(p)
      return p
    } catch {}
  }
  return null
}

async function findGoldFile(): Promise<string | null> {
  const candidates = [
    '/app/backend-runtime/HUNTER_gold.txt',
    '/app/runtime/HUNTER_gold.txt',
    join(process.cwd(), 'runtime', 'HUNTER_gold.txt'),
    join(process.cwd(), 'backend-runtime', 'HUNTER_gold.txt'),
  ]
  for (const p of candidates) {
    try {
      await access(p)
      return p
    } catch {}
  }
  return null
}

export async function GET(
  request: NextRequest,
  { params }: { params: { type: string } }
) {
  try {
    const type = params.type

    if (type === 'gold') {
      // Try the gold file first (pre-filtered by backend)
      const goldPath = await findGoldFile()
      if (goldPath) {
        const content = await readFile(goldPath, 'utf-8')
        const lines = content.split('\n').filter((l) => l.trim().length > 0)
        return new NextResponse(lines.join('\n'), {
          headers: {
            'Content-Type': 'text/plain',
            'Content-Disposition': 'attachment; filename="hunter_gold_configs.txt"',
            'X-Config-Count': String(lines.length),
          },
        })
      }
    }

    // Fall back to parsing the TSV database
    const dbPath = await findConfigDb()
    if (!dbPath) {
      return NextResponse.json(
        { error: 'Config database not found. Ensure configs have been loaded.', searched: [
          '/app/backend-runtime/', '/app/runtime/', 'runtime/', 'backend-runtime/'
        ]},
        { status: 404 }
      )
    }

    const content = await readFile(dbPath, 'utf-8')
    const lines = content.split('\n').filter((l) => l.trim().length > 0)

    const configs: ParsedConfig[] = []
    for (const line of lines) {
      const parsed = parseTsvLine(line)
      if (parsed) configs.push(parsed)
    }

    if (type === 'all') {
      const uris = configs.map((c) => c.uri)
      return new NextResponse(uris.join('\n'), {
        headers: {
          'Content-Type': 'text/plain',
          'Content-Disposition': 'attachment; filename="hunter_all_configs.txt"',
          'X-Config-Count': String(uris.length),
        },
      })
    }

    if (type === 'gold') {
      // Gold = alive, tested, non-telegram-only, has latency
      const gold = configs
        .filter((c) => c.alive && !c.telegram_only && c.latency > 0)
        .sort((a, b) => a.latency - b.latency)
        .map((c) => c.uri)
      return new NextResponse(gold.join('\n'), {
        headers: {
          'Content-Type': 'text/plain',
          'Content-Disposition': 'attachment; filename="hunter_gold_configs.txt"',
          'X-Config-Count': String(gold.length),
        },
      })
    }

    if (type === 'silver') {
      // Silver = tested but not alive, or alive but telegram-only, or untested
      const silver = configs
        .filter((c) => !c.alive || c.telegram_only || c.total_tests === 0)
        .map((c) => c.uri)
      return new NextResponse(silver.join('\n'), {
        headers: {
          'Content-Type': 'text/plain',
          'Content-Disposition': 'attachment; filename="hunter_silver_configs.txt"',
          'X-Config-Count': String(silver.length),
        },
      })
    }

    if (type === 'alive') {
      const alive = configs.filter((c) => c.alive).map((c) => c.uri)
      return new NextResponse(alive.join('\n'), {
        headers: {
          'Content-Type': 'text/plain',
          'Content-Disposition': 'attachment; filename="hunter_alive_configs.txt"',
          'X-Config-Count': String(alive.length),
        },
      })
    }

    // Single config by URI hash - return the URI directly
    if (type === 'single') {
      const uri = request.nextUrl.searchParams.get('uri')
      if (!uri) {
        return NextResponse.json({ error: 'Missing uri parameter' }, { status: 400 })
      }
      return new NextResponse(uri, {
        headers: {
          'Content-Type': 'text/plain',
          'Content-Disposition': 'attachment; filename="config.txt"',
        },
      })
    }

    return NextResponse.json({ error: `Unknown type: ${type}` }, { status: 400 })
  } catch (error) {
    const msg = error instanceof Error ? error.message : String(error)
    return NextResponse.json(
      { error: 'Failed to fetch configs', detail: msg },
      { status: 500 }
    )
  }
}
