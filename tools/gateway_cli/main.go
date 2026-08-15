// gateway_cli — диагностический клиент к TCP-gateway (порт 7777).
//
// Позволяет отправлять одиночные SetBlockAction (place/break) с корректным
// held_item и request_id, печатать BlockAck'и с человекочитаемым статусом
// и гонять многошаговые сценарии (script) для воспроизведения багов
// «срубаю-ставлю» / сети труб без необходимости входить в игру.
//
// Использование:
//
//	go run . place 400 70 401 0xF800
//	go run . break 400 69 401 --expected 0xF800
//	go run . script /tmp/scenario.txt
//	go run . --addr 127.0.0.1:7777 --player 1 place 400 70 401 0xE400
//
// Формат script-файла (по строке):
//
//	# комментарий
//	sleep 500
//	place 400 70 400 0xE400
//	break 400 69 401 --expected 0xF800
//	wait 2000            # ждать ACK до N мс (default 1500)
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"net"
	"os"
	"strconv"
	"strings"
	"time"

	"github.com/google/flatbuffers/go"
	Protocol "github.com/gtnh-platform/protocol/generated/go/Protocol"
)

const (
	kPlayerAction   = 1
	kBlockAck       = 5
	kSetBlockAction = 11
)

// pack("1110:01:0") -> 0xE400 (steam_solid_boiler), pack("1111:10:0") -> 0xF800 (fluid_pipe)
func packID(prefix string, payload uint16) uint16 {
	var p uint16
	plen := 0
	for _, c := range prefix {
		if c == '0' || c == '1' {
			p = (p << 1) | uint16(c-'0')
			plen++
		}
	}
	return (p << (16 - plen)) | payload
}

func parseItem(s string) (uint16, error) {
	s = strings.TrimSpace(s)
	if strings.HasPrefix(s, "0x") || strings.HasPrefix(s, "0X") {
		v, err := strconv.ParseUint(s[2:], 16, 16)
		if err != nil {
			return 0, fmt.Errorf("bad hex item %q: %w", s, err)
		}
		return uint16(v), nil
	}
	if strings.Contains(s, ":") {
		parts := strings.Split(s, ":")
		if len(parts) == 3 {
			p, err := strconv.ParseUint(parts[1], 2, 16)
			if err != nil {
				return 0, fmt.Errorf("bad pack item %q: %w", s, err)
			}
			plen := len(parts[1])
			if plen == 0 || plen > 16 {
				return 0, fmt.Errorf("bad pack length in %q", s)
			}
			payload, err := strconv.ParseUint(parts[2], 10, 16)
			if err != nil {
				return 0, fmt.Errorf("bad payload in %q: %w", s, err)
			}
			return uint16(p<<(16-plen)) | uint16(payload), nil
		}
	}
	v, err := strconv.ParseUint(s, 10, 16)
	if err != nil {
		return 0, fmt.Errorf("bad item %q (use 0xHEX, decimal, or pack like 1110:01:0)", s)
	}
	return uint16(v), nil
}

type client struct {
	conn net.Conn
	nextRequestID uint32
}

func (c *client) send(action *flatbuffers.Builder, fbData []byte, desc string) {
	frame := make([]byte, 4+1+len(fbData))
	binary.BigEndian.PutUint32(frame[0:4], uint32(1+len(fbData)))
	frame[4] = kSetBlockAction
	copy(frame[5:], fbData)
	if _, err := c.conn.Write(frame); err != nil {
		fmt.Println("  WRITE FAIL:", err)
		return
	}
	fmt.Println(desc)
}

func (c *client) place(x, y, z int32, item uint16, expected uint16) uint32 {
	req := c.nextRequestID
	c.nextRequestID++
	b := flatbuffers.NewBuilder(64)
	Protocol.SetBlockActionStart(b)
	Protocol.SetBlockActionAddPlayerId(b, 0)
	Protocol.SetBlockActionAddAction(b, Protocol.PlayerActionTypeRIGHT_MOUSE_CLICK)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetBlockActionAddPos(b, pos)
	Protocol.SetBlockActionAddExpectedBlockId(b, expected)
	Protocol.SetBlockActionAddNewBlockId(b, item)
	Protocol.SetBlockActionAddHeldItem(b, item)
	Protocol.SetBlockActionAddRequestId(b, req)
	action := Protocol.SetBlockActionEnd(b)
	b.Finish(action)
	c.send(b, b.FinishedBytes(), fmt.Sprintf("place  (%d,%d,%d) item=0x%04X expected=0x%04X req=%d", x, y, z, item, expected, req))
	return req
}

func (c *client) brk(x, y, z int32, expected uint16) uint32 {
	req := c.nextRequestID
	c.nextRequestID++
	b := flatbuffers.NewBuilder(64)
	Protocol.SetBlockActionStart(b)
	Protocol.SetBlockActionAddPlayerId(b, 0)
	Protocol.SetBlockActionAddAction(b, Protocol.PlayerActionTypeLEFT_MOUSE_CLICK)
	pos := Protocol.CreateVec3i(b, x, y, z)
	Protocol.SetBlockActionAddPos(b, pos)
	Protocol.SetBlockActionAddExpectedBlockId(b, expected)
	Protocol.SetBlockActionAddNewBlockId(b, 0)
	Protocol.SetBlockActionAddHeldItem(b, 0)
	Protocol.SetBlockActionAddRequestId(b, req)
	action := Protocol.SetBlockActionEnd(b)
	b.Finish(action)
	c.send(b, b.FinishedBytes(), fmt.Sprintf("break  (%d,%d,%d) expected=0x%04X req=%d", x, y, z, expected, req))
	return req
}

func statusName(s Protocol.BlockAckStatus) string {
	switch s {
	case 0:
		return "REJECTED"
	case 1:
		return "ACCEPTED"
	case 2:
		return "CONFLICT"
	}
	return fmt.Sprintf("UNKNOWN(%d)", uint8(s))
}

func (c *client) startReader() {
	go func() {
		header := make([]byte, 5)
		for {
			if _, err := c.conn.Read(header); err != nil {
				return
			}
			payloadLen := binary.BigEndian.Uint32(header[:4])
			if payloadLen < 1 {
				continue
			}
			payload := make([]byte, payloadLen-1)
			if _, err := c.conn.Read(payload); err != nil {
				return
			}
			if header[4] == kBlockAck {
				ack := Protocol.GetRootAsBlockAck(payload, 0)
				var pos Protocol.Vec3i
				ack.Pos(&pos)
				fmt.Printf("  ACK pos=(%d,%d,%d) status=%s (req=%d)\n",
					pos.X(), pos.Y(), pos.Z(), statusName(ack.Status()), ack.RequestId())
			}
		}
	}()
}

func waitAck(ms int) {
	time.Sleep(time.Duration(ms) * time.Millisecond)
}

func main() {
	addr := flag.String("addr", "127.0.0.1:7777", "gateway TCP address")
	timeout := flag.Int("timeout", 1500, "default ACK wait (ms) after each action")
	flag.Parse()
	args := flag.Args()
	if len(args) == 0 {
		fmt.Println("usage: gateway_cli [--addr HOST:PORT] <place|break|script> ...")
		os.Exit(2)
	}

	conn, err := net.DialTimeout("tcp", *addr, 5*time.Second)
	if err != nil {
		fmt.Println("connect failed:", err)
		os.Exit(1)
	}
	defer conn.Close()
	fmt.Printf("connected to gateway %s\n", *addr)

	c := &client{conn: conn}
	c.startReader()
	time.Sleep(200 * time.Millisecond)

	run := func(cmd string, x, y, z int32, item, expected uint16) {
		switch cmd {
		case "place":
			c.place(x, y, z, item, expected)
		case "break":
			c.brk(x, y, z, expected)
		}
		waitAck(*timeout)
	}

	switch args[0] {
	case "place":
		if len(args) < 5 {
			fmt.Println("usage: gateway_cli place <x> <y> <z> <item>")
			os.Exit(2)
		}
		x, _ := strconv.ParseInt(args[1], 10, 32)
		y, _ := strconv.ParseInt(args[2], 10, 32)
		z, _ := strconv.ParseInt(args[3], 10, 32)
		item, err := parseItem(args[4])
		if err != nil {
			fmt.Println(err)
			os.Exit(2)
		}
		run("place", int32(x), int32(y), int32(z), item, 0)

	case "break":
		if len(args) < 4 {
			fmt.Println("usage: gateway_cli break <x> <y> <z> [--expected 0xN]")
			os.Exit(2)
		}
		x, _ := strconv.ParseInt(args[1], 10, 32)
		y, _ := strconv.ParseInt(args[2], 10, 32)
		z, _ := strconv.ParseInt(args[3], 10, 32)
		expected := uint16(0)
		for i := 4; i < len(args); i++ {
			if args[i] == "--expected" && i+1 < len(args) {
				item, err := parseItem(args[i+1])
				if err != nil {
					fmt.Println(err)
					os.Exit(2)
				}
				expected = item
			}
		}
		run("break", int32(x), int32(y), int32(z), 0, expected)

	case "script":
		if len(args) < 2 {
			fmt.Println("usage: gateway_cli script <file>")
			os.Exit(2)
		}
		data, err := os.ReadFile(args[1])
		if err != nil {
			fmt.Println("read script:", err)
			os.Exit(1)
		}
		for _, line := range strings.Split(string(data), "\n") {
			line = strings.TrimSpace(line)
			if line == "" || strings.HasPrefix(line, "#") {
				continue
			}
			fields := strings.Fields(line)
			switch fields[0] {
			case "sleep":
				ms, _ := strconv.Atoi(fields[1])
				fmt.Printf("== sleep %dms ==\n", ms)
				waitAck(ms)
			case "wait":
				ms, _ := strconv.Atoi(fields[1])
				fmt.Printf("== wait %dms ==\n", ms)
				waitAck(ms)
			case "place", "break":
				if len(fields) < 5 {
					fmt.Println("bad script line:", line)
					continue
				}
				x, _ := strconv.ParseInt(fields[1], 10, 32)
				y, _ := strconv.ParseInt(fields[2], 10, 32)
				z, _ := strconv.ParseInt(fields[3], 10, 32)
				expected := uint16(0)
				if fields[0] == "break" {
					item, err := parseItem(fields[4])
					if err != nil {
						fmt.Println("bad item:", err)
						continue
					}
					expected = item
				} else {
					item, err := parseItem(fields[4])
					if err != nil {
						fmt.Println("bad item:", err)
						continue
					}
					run("place", int32(x), int32(y), int32(z), item, expected)
					continue
				}
				run("break", int32(x), int32(y), int32(z), 0, expected)
			default:
				fmt.Println("unknown script cmd:", fields[0])
			}
		}

	default:
		fmt.Println("unknown command:", args[0])
		os.Exit(2)
	}
	fmt.Println("done")
}
