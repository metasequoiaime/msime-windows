// glossctl 用纯 Go 标准库直接调用 sqlite3.dll，不引入任何第三方依赖。
//
// 为什么不是 Python：旧脚本要跑 225 万行，解释器逐行开销远大于 SQLite 本身；
// 为什么不是 cgo：这台机器没有 gcc，而 sqlite3.dll 本来就随输入法一起分发。
// 结果是单个 exe，复制到哪都能跑，启动即用。
package main

import (
	"fmt"
	"os"
	"runtime"
	"strconv"
	"strings"
	"syscall"
	"time"
	"unsafe"
)

const (
	sqliteOK   = 0
	sqliteRow  = 100
	sqliteDone = 101

	openReadOnly  = 0x00000001
	openReadWrite = 0x00000002
	openCreate    = 0x00000004
	openNoMutex   = 0x00010000
)

var (
	sqlite3             = syscall.NewLazyDLL("sqlite3.dll")
	procOpen            = sqlite3.NewProc("sqlite3_open_v2")
	procClose           = sqlite3.NewProc("sqlite3_close")
	procErrmsg          = sqlite3.NewProc("sqlite3_errmsg")
	procExec            = sqlite3.NewProc("sqlite3_exec")
	procPrepare         = sqlite3.NewProc("sqlite3_prepare_v2")
	procStep            = sqlite3.NewProc("sqlite3_step")
	procFinalize        = sqlite3.NewProc("sqlite3_finalize")
	procColumnText      = sqlite3.NewProc("sqlite3_column_text")
	procColumnBytes     = sqlite3.NewProc("sqlite3_column_bytes")
	procColumnInt64     = sqlite3.NewProc("sqlite3_column_int64")
	procBindText        = sqlite3.NewProc("sqlite3_bind_text")
	procBindInt64       = sqlite3.NewProc("sqlite3_bind_int64")
	procReset           = sqlite3.NewProc("sqlite3_reset")
	procChanges         = sqlite3.NewProc("sqlite3_changes")
)

type DB struct {
	handle uintptr
	path   string
}

type Stmt struct {
	handle uintptr
}

func cString(p uintptr) string {
	if p == 0 {
		return ""
	}
	var out []byte
	for i := uintptr(0); ; i++ {
		b := *(*byte)(unsafe.Pointer(p + i))
		if b == 0 {
			break
		}
		out = append(out, b)
	}
	return string(out)
}

func open(path string, readonly bool) (*DB, error) {
	p, err := syscall.BytePtrFromString(path)
	if err != nil {
		return nil, err
	}
	flags := uintptr(openReadWrite | openNoMutex)
	if readonly {
		flags = openReadOnly | openNoMutex
	} else {
		flags |= openCreate
	}
	var handle uintptr
	rc, _, _ := procOpen.Call(uintptr(unsafe.Pointer(p)), uintptr(unsafe.Pointer(&handle)), flags, 0)
	db := &DB{handle: handle, path: path}
	if rc != sqliteOK {
		msg := db.errmsg()
		if handle != 0 {
			procClose.Call(handle)
		}
		return nil, fmt.Errorf("打开 %s 失败：%s", path, msg)
	}
	return db, nil
}

func (d *DB) errmsg() string {
	if d.handle == 0 {
		return "sqlite3.dll 未加载"
	}
	p, _, _ := procErrmsg.Call(d.handle)
	return cString(p)
}

func (d *DB) close() {
	if d.handle != 0 {
		procClose.Call(d.handle)
		d.handle = 0
	}
}

func (d *DB) exec(sql string) error {
	p, err := syscall.BytePtrFromString(sql)
	if err != nil {
		return err
	}
	rc, _, _ := procExec.Call(d.handle, uintptr(unsafe.Pointer(p)), 0, 0, 0)
	if rc != sqliteOK {
		return fmt.Errorf("%s（SQL: %s）", d.errmsg(), firstLine(sql))
	}
	return nil
}

func (d *DB) prepare(sql string) (*Stmt, error) {
	p, err := syscall.BytePtrFromString(sql)
	if err != nil {
		return nil, err
	}
	var handle uintptr
	rc, _, _ := procPrepare.Call(d.handle, uintptr(unsafe.Pointer(p)), ^uintptr(0), uintptr(unsafe.Pointer(&handle)), 0)
	if rc != sqliteOK {
		return nil, fmt.Errorf("%s（SQL: %s）", d.errmsg(), firstLine(sql))
	}
	return &Stmt{handle: handle}, nil
}

func (d *DB) changes() int64 {
	v, _, _ := procChanges.Call(d.handle)
	return int64(v)
}

func (s *Stmt) finalize() {
	if s.handle != 0 {
		procFinalize.Call(s.handle)
		s.handle = 0
	}
}

func (s *Stmt) step() (bool, error) {
	rc, _, _ := procStep.Call(s.handle)
	switch rc {
	case sqliteRow:
		return true, nil
	case sqliteDone:
		return false, nil
	default:
		return false, fmt.Errorf("sqlite3_step 返回 %d", rc)
	}
}

func (s *Stmt) text(col int) string {
	p, _, _ := procColumnText.Call(s.handle, uintptr(col))
	if p == 0 {
		return ""
	}
	n, _, _ := procColumnBytes.Call(s.handle, uintptr(col))
	if n == 0 {
		return ""
	}
	return string(unsafe.Slice((*byte)(unsafe.Pointer(p)), int(n)))
}

func (s *Stmt) int64(col int) int64 {
	v, _, _ := procColumnInt64.Call(s.handle, uintptr(col))
	return int64(v)
}

func (s *Stmt) bindText(index int, value string) error {
	p, err := syscall.BytePtrFromString(value)
	if err != nil {
		return err
	}
	// SQLITE_TRANSIENT：让 SQLite 自己拷一份。传 SQLITE_STATIC 的话，
	// Go 这边上一步分配的 C 字符串可能在 step 之前就被 GC 回收。
	rc, _, _ := procBindText.Call(s.handle, uintptr(index), uintptr(unsafe.Pointer(p)), ^uintptr(0), ^uintptr(0))
	if rc != sqliteOK {
		return fmt.Errorf("绑定文本失败：rc=%d", rc)
	}
	return nil
}

func (s *Stmt) reset() {
	procReset.Call(s.handle)
}

func (s *Stmt) bindInt64(index int, value int64) {
	procBindInt64.Call(s.handle, uintptr(index), uintptr(value))
}

func firstLine(s string) string {
	if i := strings.IndexByte(s, '\n'); i >= 0 {
		return s[:i]
	}
	return s
}

func human(n int64) string {
	switch {
	case n >= 100000000:
		return fmt.Sprintf("%.2f 亿", float64(n)/100000000)
	case n >= 10000:
		return fmt.Sprintf("%.2f 万", float64(n)/10000)
	default:
		return strconv.FormatInt(n, 10)
	}
}

func ensureSchema(db *DB) error {
	// 与 engine 侧 ensure_schema 保持完全一致：COLLATE BINARY + WITHOUT ROWID。
	// 两边各建一种形状，导出的库并进正式库时虽然能按列读，但文件里会多一张形状不同的表。
	return db.exec(`CREATE TABLE IF NOT EXISTS zh_glosses (
		chinese TEXT COLLATE BINARY NOT NULL,
		lang TEXT COLLATE BINARY NOT NULL,
		gloss TEXT NOT NULL,
		PRIMARY KEY (chinese, lang)) WITHOUT ROWID`)
}

func tableExists(db *DB, name string) (bool, error) {
	stmt, err := db.prepare("SELECT count(*) FROM sqlite_master WHERE type='table' AND name=?1")
	if err != nil {
		return false, err
	}
	defer stmt.finalize()
	if err := stmt.bindText(1, name); err != nil {
		return false, err
	}
	ok, err := stmt.step()
	if err != nil || !ok {
		return false, err
	}
	return stmt.int64(0) > 0, nil
}

type langCount struct {
	Lang  string
	Count int64
}

func languages(db *DB) ([]langCount, int64, error) {
	stmt, err := db.prepare("SELECT lang, count(*) FROM zh_glosses GROUP BY lang ORDER BY lang")
	if err != nil {
		return nil, 0, err
	}
	defer stmt.finalize()
	var out []langCount
	var total int64
	for {
		ok, err := stmt.step()
		if err != nil {
			return nil, 0, err
		}
		if !ok {
			break
		}
		c := langCount{Lang: stmt.text(0), Count: stmt.int64(1)}
		total += c.Count
		out = append(out, c)
	}
	return out, total, nil
}

func report(db *DB, label string) (int64, error) {
	ok, err := tableExists(db, "zh_glosses")
	if err != nil {
		return 0, err
	}
	if !ok {
		return 0, fmt.Errorf("%s 里没有 zh_glosses，可能不是本地释义包", label)
	}
	langs, total, err := languages(db)
	if err != nil {
		return 0, err
	}
	fmt.Printf("%s：%s 条，%d 个语种\n", label, human(total), len(langs))
	for _, l := range langs {
		fmt.Printf("  %-4s %10s\n", l.Lang, human(l.Count))
	}
	return total, nil
}

func progress(done int64, started time.Time) {
	elapsed := time.Since(started).Seconds()
	if elapsed <= 0 {
		elapsed = 0.001
	}
	fmt.Printf("\r  已处理 %s 条（%s 条/秒）      ", human(done), human(int64(float64(done)/elapsed)))
}

func copyRows(src, dst *DB, batch int) (int64, error) {
	read, err := src.prepare("SELECT chinese, lang, gloss FROM zh_glosses")
	if err != nil {
		return 0, err
	}
	defer read.finalize()

	if err := dst.exec("BEGIN"); err != nil {
		return 0, err
	}
	insert, err := dst.prepare("INSERT OR IGNORE INTO zh_glosses(chinese, lang, gloss) VALUES(?1, ?2, ?3)")
	if err != nil {
		dst.exec("ROLLBACK")
		return 0, err
	}
	defer insert.finalize()

	var copied int64
	started := time.Now()
	for {
		ok, err := read.step()
		if err != nil {
			dst.exec("ROLLBACK")
			return copied, err
		}
		if !ok {
			break
		}
		if err := insert.bindText(1, read.text(0)); err != nil {
			dst.exec("ROLLBACK")
			return copied, err
		}
		if err := insert.bindText(2, read.text(1)); err != nil {
			dst.exec("ROLLBACK")
			return copied, err
		}
		if err := insert.bindText(3, read.text(2)); err != nil {
			dst.exec("ROLLBACK")
			return copied, err
		}
		if _, err := insert.step(); err != nil {
			dst.exec("ROLLBACK")
			return copied, err
		}
		insert.reset()
		copied++
		if copied%int64(batch) == 0 {
			progress(copied, started)
		}
	}
	insert.finalize()
	if err := dst.exec("COMMIT"); err != nil {
		return copied, err
	}
	progress(copied, started)
	fmt.Println()
	return copied, nil
}

func requireFile(path string) {
	if st, err := os.Stat(path); err != nil || st.IsDir() {
		fatal("找不到文件：%s", path)
	}
}

func fatal(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "错误："+format+"\n", args...)
	os.Exit(1)
}

func loadSQLite() {
	if err := sqlite3.Load(); err != nil {
		hint := "把 sqlite3.dll 放到 glossctl.exe 同目录，或加入 PATH"
		if runtime.GOARCH == "amd64" {
			hint += "（需要 64 位版本）"
		}
		fatal("加载 sqlite3.dll 失败：%v\n%s", err, hint)
	}
}

// ---- 子命令 ----

func cmdStats(args []string) {
	if len(args) < 1 {
		fatal("用法：glossctl stats --db <english.db>")
	}
	path := flagValue(args, "--db")
	requireFile(path)
	db, err := open(path, true)
	if err != nil {
		fatal("%v", err)
	}
	defer db.close()
	if _, err := report(db, path); err != nil {
		fatal("%v", err)
	}
}

func cmdExport(args []string) {
	source := flagValue(args, "--source")
	out := flagValue(args, "--out")
	force := hasFlag(args, "--force")
	if source == "" || out == "" {
		fatal("用法：glossctl export --source <旧 english.db> --out <local_glosses.db> [--force]")
	}
	requireFile(source)
	if _, err := os.Stat(out); err == nil && !force {
		fatal("%s 已存在，加 --force 覆盖", out)
	}
	if force {
		os.Remove(out)
	}

	src, err := open(source, true)
	if err != nil {
		fatal("%v", err)
	}
	defer src.close()
	if _, err := report(src, "源库"); err != nil {
		fatal("%v", err)
	}

	dst, err := open(out, false)
	if err != nil {
		fatal("%v", err)
	}
	defer dst.close()
	dst.exec("PRAGMA journal_mode=OFF")
	dst.exec("PRAGMA synchronous=OFF")
	if err := ensureSchema(dst); err != nil {
		fatal("建表失败：%v", err)
	}
	copied, err := copyRows(src, dst, 20000)
	if err != nil {
		fatal("复制中断：%v", err)
	}
	dst.exec("VACUUM")
	info, _ := os.Stat(out)
	size := float64(0)
	if info != nil {
		size = float64(info.Size()) / (1024 * 1024)
	}
	fmt.Printf("完成：%s（%s 条，%.1f MB）\n", out, human(copied), size)
}

func cmdMerge(args []string) {
	source := flagValue(args, "--source")
	target := flagValue(args, "--target")
	noBackup := hasFlag(args, "--no-backup")
	if source == "" || target == "" {
		fatal("用法：glossctl merge --source <旧 english.db> --target <english.db> [--no-backup]")
	}
	requireFile(source)
	requireFile(target)

	src, err := open(source, true)
	if err != nil {
		fatal("%v", err)
	}
	defer src.close()
	if _, err := report(src, "源库"); err != nil {
		fatal("%v", err)
	}

	if !noBackup {
		backup := target + ".bak-" + time.Now().Format("20060102-150405")
		if err := copyFile(target, backup); err != nil {
			fatal("备份失败：%v", err)
		}
		fmt.Println("备份 " + backup)
	}

	dst, err := open(target, false)
	if err != nil {
		fatal("%v", err)
	}
	defer dst.close()
	dst.exec("PRAGMA journal_mode=WAL")
	dst.exec("PRAGMA synchronous=NORMAL")
	if err := ensureSchema(dst); err != nil {
		fatal("建表失败：%v", err)
	}
	_, before, err := languages(dst)
	if err != nil {
		fatal("%v", err)
	}
	copied, err := copyRows(src, dst, 20000)
	if err != nil {
		fatal("复制中断：%v", err)
	}
	_, after, err := languages(dst)
	if err != nil {
		fatal("%v", err)
	}
	dst.exec("PRAGMA wal_checkpoint(TRUNCATE)")
	fmt.Printf("完成：%s → %s 条（尝试写入 %s，新增 %s）\n", human(before), human(after), human(copied), human(after-before))
}

func cmdVerify(args []string) {
	path := flagValue(args, "--db")
	if path == "" {
		fatal("用法：glossctl verify --db <english.db>")
	}
	requireFile(path)
	db, err := open(path, true)
	if err != nil {
		fatal("%v", err)
	}
	defer db.close()
	total, err := report(db, path)
	if err != nil {
		fatal("%v", err)
	}

	checks := []struct {
		label string
		sql   string
	}{
		{"空词条", "SELECT count(*) FROM zh_glosses WHERE chinese IS NULL OR chinese=''"},
		{"空译文", "SELECT count(*) FROM zh_glosses WHERE gloss IS NULL OR gloss=''"},
		{"空语种", "SELECT count(*) FROM zh_glosses WHERE lang IS NULL OR lang=''"},
		{"译文超长(>200)", "SELECT count(*) FROM zh_glosses WHERE length(gloss) > 200"},
	}
	problems := int64(0)
	for _, c := range checks {
		stmt, err := db.prepare(c.sql)
		if err != nil {
			fatal("%v", err)
		}
		if ok, err := stmt.step(); err == nil && ok {
			n := stmt.int64(0)
			fmt.Printf("  %-16s %s\n", c.label, human(n))
			problems += n
		}
		stmt.finalize()
	}
	fmt.Printf("总计 %s 条，问题 %s 条\n", human(total), human(problems))
	if problems > 0 {
		os.Exit(2)
	}
}

func copyFile(from, to string) error {
	in, err := os.Open(from)
	if err != nil {
		return err
	}
	defer in.Close()
	out, err := os.Create(to)
	if err != nil {
		return err
	}
	if _, err := out.ReadFrom(in); err != nil {
		out.Close()
		return err
	}
	return out.Close()
}

func flagValue(args []string, name string) string {
	for i, a := range args {
		if a == name && i+1 < len(args) {
			return args[i+1]
		}
		if strings.HasPrefix(a, name+"=") {
			return strings.TrimPrefix(a, name+"=")
		}
	}
	return ""
}

func hasFlag(args []string, name string) bool {
	for _, a := range args {
		if a == name {
			return true
		}
	}
	return false
}

func usage() {
	fmt.Println(`glossctl —— 本地释义包工具（Go + sqlite3.dll，无第三方依赖）

  glossctl stats  --db <english.db>
  glossctl export --source <旧 english.db> --out <local_glosses.db> [--force]
  glossctl merge  --source <旧 english.db> --target <english.db> [--no-backup]
  glossctl verify --db <english.db>

退出码：0 正常，1 出错，2 校验发现问题`)
}

func main() {
	if len(os.Args) < 2 {
		usage()
		return
	}
	loadSQLite()
	cmd := os.Args[1]
	args := os.Args[2:]
	switch cmd {
	case "stats":
		cmdStats(args)
	case "export":
		cmdExport(args)
	case "merge":
		cmdMerge(args)
	case "verify":
		cmdVerify(args)
	case "-h", "--help", "help":
		usage()
	default:
		usage()
		os.Exit(1)
	}
}
