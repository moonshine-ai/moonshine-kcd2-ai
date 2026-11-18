// Receives the bug reports the Moonshine window sends, keeps each one in R2 for
// 90 days and emails a link to it.
//
//   POST /kcd2/report                               the zip is the body
//   GET  /kcd2/r/<day>/<ref>-<secret>/logs.zip      the zip back again
//   GET  /kcd2/r/<day>/<ref>-<secret>/report.json   what the player wrote
//
// Anybody can read this file and anybody can post to the address, so nothing
// here is trusted and everything is bounded. The limits are deliberately
// generous: they exist to cap the bill, not to keep anybody out.

// ---- Limits -----------------------------------------------------------------

// One report. The logs of a long session zip to a megabyte or two.
const MAX_REPORT_BYTES = 25 * 1024 * 1024;
// Everybody together, per UTC day. Past either, the app is told to try tomorrow.
const MAX_REPORTS_PER_DAY = 200;
const MAX_BYTES_PER_DAY = 2 * 1024 * 1024 * 1024;
// Past this many in a day the reports are still kept, but only one more email
// goes out, to say so.
const MAX_EMAILS_PER_DAY = 50;
// Reports per minute from one address: "ratelimits" in wrangler.jsonc.

// What the player may write, in characters. The app holds to the same numbers.
const MAX_NAME = 200;
const MAX_EMAIL = 254;
const MAX_DESCRIPTION = 4000;
const MAX_VERSION = 40;

// ---- Names ------------------------------------------------------------------

const PRODUCT = "kcd2";
const FROM = { email: "bug-reports@moonshine.ai", name: "Moonshine bug reports" };
// The header the app puts the form in: base64 of UTF-8 JSON with name, email,
// description and version, all optional.
const FORM_HEADER = "x-moonshine-report";
// No I, L, O, 0 or 1: a reference gets read aloud and typed.
const REF_LETTERS = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
const STORED = /^\/kcd2\/r\/(\d{4}-\d{2}-\d{2})\/([A-HJ-KM-NP-Z2-9]{8}-[0-9a-f]{32})\/(logs\.zip|report\.json)$/;

export default {
  async fetch(request, env) {
    try {
      return await route(request, env);
    } catch (error) {
      console.error(JSON.stringify({ event: "failed", message: String(error?.message ?? error) }));
      return refuse(500, "failed", "The report service had a problem. Please try again in a while.");
    }
  },
};

async function route(request, env) {
  const url = new URL(request.url);
  if (url.pathname === `/${PRODUCT}/report`) {
    if (request.method !== "POST") return refuse(405, "method", "Reports are sent with POST.");
    return receive(request, env, url);
  }
  const stored = STORED.exec(url.pathname);
  if (stored && request.method === "GET") return serve(env, stored[1], stored[2], stored[3]);
  if (url.pathname === "/robots.txt") return new Response("User-agent: *\nDisallow: /\n");
  if (url.pathname === "/") return new Response("Moonshine bug reports.\n");
  return refuse(404, "not-found", "There is nothing here.");
}

// ---- Receiving --------------------------------------------------------------

async function receive(request, env, url) {
  // The size has to be known before a byte is read: R2 needs it to take the
  // body as a stream, and it is how an oversized report is refused for free.
  const declared = request.headers.get("content-length");
  const size = declared === null ? NaN : Number(declared);
  if (!Number.isInteger(size)) {
    return refuse(411, "no-length", "The report came without a size.");
  }
  if (size <= 0 || request.body === null) {
    return refuse(400, "empty", "The report was empty.");
  }
  if (size > MAX_REPORT_BYTES) {
    return refuse(413, "too-big", `The report is larger than ${megabytes(MAX_REPORT_BYTES)}.`);
  }

  const address = request.headers.get("cf-connecting-ip") ?? "unknown";
  const { success } = await env.PER_IP.limit({ key: address });
  if (!success) {
    return refuse(429, "too-fast", "Too many reports in a minute. Please wait a minute and send it again.");
  }

  const form = read_form(request.headers.get(FORM_HEADER));
  if (form === null) return refuse(400, "bad-form", "The report's details could not be read.");

  const now = new Date();
  const day = now.toISOString().slice(0, 10);
  const today = await usage(env.REPORTS, day);
  if (today.reports >= MAX_REPORTS_PER_DAY || today.bytes + size > MAX_BYTES_PER_DAY) {
    console.warn(JSON.stringify({ event: "day-full", day, ...today }));
    return refuse(503, "day-full", "Too many reports have come in today. Please send it again tomorrow.");
  }

  const ref = reference();
  const name = `${ref}-${secret()}`;
  const folder = `${PRODUCT}/${day}/${name}`;

  // Straight through to the bucket: the zip is never held in memory.
  const zip = await env.REPORTS.put(`${folder}/logs.zip`, request.body, {
    httpMetadata: { contentType: "application/zip" },
  });
  const report = {
    ref,
    received: now.toISOString(),
    ...form,
    size: zip.size,
    country: request.cf?.country ?? "",
    agent: clean(request.headers.get("user-agent"), 200, false),
  };
  await env.REPORTS.put(`${folder}/report.json`, JSON.stringify(report, null, 2) + "\n", {
    httpMetadata: { contentType: "application/json; charset=utf-8" },
  });

  // The report is safe by now, so a failure to say so by email is not the
  // player's problem: they still get their reference.
  let emailed = false;
  try {
    const link = `${url.origin}/${PRODUCT}/r/${day}/${name}/logs.zip`;
    if (today.reports < MAX_EMAILS_PER_DAY) {
      await env.EMAIL.send(announcement(env, report, link));
      emailed = true;
    } else if (today.reports === MAX_EMAILS_PER_DAY) {
      await env.EMAIL.send(cap_notice(env, day));
      emailed = true;
    }
  } catch (error) {
    console.error(JSON.stringify({
      event: "email-failed", ref, code: error?.code, message: String(error?.message ?? error),
    }));
  }

  console.log(JSON.stringify({
    event: "report", ref, size: zip.size, country: report.country, version: report.version, emailed,
  }));
  return json(200, { ref, emailed });
}

// How much has come in on this UTC day, counted from the bucket itself so there
// is no counter to keep. At the daily limit this is one listing of 400 objects.
async function usage(bucket, day) {
  let reports = 0;
  let bytes = 0;
  let cursor;
  do {
    const page = await bucket.list({ prefix: `${PRODUCT}/${day}/`, cursor });
    for (const object of page.objects) {
      if (!object.key.endsWith("/logs.zip")) continue;
      reports += 1;
      bytes += object.size;
    }
    cursor = page.truncated ? page.cursor : undefined;
  } while (cursor);
  return { reports, bytes };
}

// The form, or null if a header was sent and is not what the app sends. No
// header at all is a report with nothing written, which is allowed.
function read_form(header) {
  const form = { name: "", email: "", description: "", version: "" };
  if (header === null || header.trim() === "") return form;
  let fields;
  try {
    const bytes = Uint8Array.from(atob(header.trim()), (c) => c.charCodeAt(0));
    fields = JSON.parse(new TextDecoder("utf-8", { fatal: true }).decode(bytes));
  } catch {
    return null;
  }
  if (fields === null || typeof fields !== "object" || Array.isArray(fields)) return null;
  form.name = clean(fields.name, MAX_NAME, false);
  form.email = clean(fields.email, MAX_EMAIL, false);
  form.description = clean(fields.description, MAX_DESCRIPTION, true);
  form.version = clean(fields.version, MAX_VERSION, false);
  return form;
}

// A string of at most `limit` characters with the control characters out of it;
// line breaks stay only where `lines` says so. Anything not a string is "".
function clean(value, limit, lines) {
  if (typeof value !== "string") return "";
  let text = value.replace(/\r\n?/g, "\n");
  text = lines
    ? text.replace(/[\u0000-\u0009\u000b-\u001f\u007f]/g, " ")
    : text.replace(/[\u0000-\u001f\u007f]/g, " ");
  return Array.from(text.trim()).slice(0, limit).join("").trim();
}

function reference() {
  const bytes = crypto.getRandomValues(new Uint8Array(8));
  return Array.from(bytes, (byte) => REF_LETTERS[byte % REF_LETTERS.length]).join("");
}

// 128 random bits. Knowing them is the only thing that opens a report.
function secret() {
  const bytes = crypto.getRandomValues(new Uint8Array(16));
  return Array.from(bytes, (byte) => byte.toString(16).padStart(2, "0")).join("");
}

// ---- The email --------------------------------------------------------------

function announcement(env, report, link) {
  const words = report.description.replace(/\s+/g, " ").trim();
  const subject = `Moonshine KCD2 report ${report.ref}` +
    (words ? `: ${words.length > 70 ? words.slice(0, 70).trimEnd() + "..." : words}` : "");
  const rows = [
    ["Reference", report.ref],
    ["Name", report.name || "(not given)"],
    ["Email", report.email || "(not given)"],
    ["Version", report.version || "(not given)"],
    ["Size", megabytes(report.size)],
    ["Country", report.country || "(unknown)"],
    ["Received", report.received.replace("T", " ").slice(0, 16) + " UTC"],
  ];
  const description = report.description || "(nothing written)";
  const caution =
    "Kept for 90 days. The zip came from the internet: read what is in it, run nothing from it.";

  const text =
    rows.map(([label, value]) => `${label}: ${value}`).join("\n") +
    `\n\nWhat happened:\n${description}\n\nLogs: ${link}\n\n${caution}\n`;
  const html =
    "<table>" +
    rows.map(([label, value]) =>
      `<tr><td style="padding:0 1em 0 0;color:#666">${escape(label)}</td><td>${escape(value)}</td></tr>`,
    ).join("") +
    "</table>" +
    `<p style="color:#666;margin-bottom:0">What happened</p>` +
    `<p style="white-space:pre-wrap;margin-top:0.3em">${escape(description)}</p>` +
    `<p><a href="${escape(link)}">Download the logs</a> (${escape(megabytes(report.size))})</p>` +
    `<p style="color:#666">${escape(caution)}</p>`;

  const message = { to: env.REPORT_TO, from: FROM, subject, text, html };
  // Only an address that looks like one: this goes into a header.
  if (/^[^\s@<>"',;:]+@[^\s@<>"',;:]+\.[^\s@<>"',;:]+$/.test(report.email)) {
    message.replyTo = report.email;
  }
  return message;
}

function cap_notice(env, day) {
  const text =
    `More than ${MAX_EMAILS_PER_DAY} bug reports have come in on ${day} (UTC), so this is the last ` +
    "email about them today. Reports are still being accepted and kept, up to " +
    `${MAX_REPORTS_PER_DAY} a day; the rest of today's are in the bucket under ${PRODUCT}/${day}/.\n`;
  return {
    to: env.REPORT_TO,
    from: FROM,
    subject: `Moonshine KCD2 reports: more than ${MAX_EMAILS_PER_DAY} today, no more emails until tomorrow`,
    text,
    html: `<p>${escape(text)}</p>`,
  };
}

function escape(text) {
  return String(text)
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
}

function megabytes(bytes) {
  const mb = bytes / (1024 * 1024);
  return mb >= 10 ? `${Math.round(mb)} MB` : mb >= 0.1 ? `${mb.toFixed(1)} MB` : `${Math.ceil(bytes / 1024)} KB`;
}

// ---- Reading a report back ---------------------------------------------------

async function serve(env, day, name, file) {
  const object = await env.REPORTS.get(`${PRODUCT}/${day}/${name}/${file}`);
  if (object === null) {
    return refuse(404, "not-found", "There is no such report. They are deleted after 90 days.");
  }
  const headers = new Headers({
    "cache-control": "private, no-store",
    "x-content-type-options": "nosniff",
    "x-robots-tag": "noindex",
  });
  if (file === "logs.zip") {
    headers.set("content-type", "application/zip");
    headers.set("content-disposition", `attachment; filename="Moonshine-report-${name.slice(0, 8)}.zip"`);
  } else {
    headers.set("content-type", "application/json; charset=utf-8");
  }
  return new Response(object.body, { headers });
}

// ---- Answers ----------------------------------------------------------------

function json(status, body) {
  return new Response(JSON.stringify(body) + "\n", {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });
}

// `message` is written for the player: the app shows it as it is.
function refuse(status, error, message) {
  return json(status, { error, message });
}
