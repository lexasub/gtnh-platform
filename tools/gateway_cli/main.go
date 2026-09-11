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
//	go run . open 277 66 128     # состояние машины (BlockEntityUpdate: энергия, слоты)
//	go run . pipe 278 66 128     # содержимое трубы (PipeContentsResp)
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
	kPlayerAction      = 1
	kBlockAck          = 5
	kBlockEntityUpdate = 8
	kSetBlockAction    = 11
	kSetMachineSlot    = 15
	kSetMachineSlotResp = 16
	kMachineOpenReq    = 18
	kMachineCloseReq   = 46
	kPipeContentsReq   = 48
	kPipeContentsResp  = 49
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
	conn          net.Conn
	nextRequestID uint32
	watchPos      bool
	watchX        int32
	watchY        int32
	watchZ        int32
}

func (c *client) sendRaw(msgType byte, fbData []byte, desc string) {
	frame := make([]byte, 4+1+len(fbData))
	binary.BigEndian.PutUint32(frame[0:4], uint32(1+len(fbData)))
	frame[4] = msgType
	copy(frame[5:], fbData)
	if _, err := c.conn.Write(frame); err != nil {
		fmt.Println("  WRITE FAIL:", err)
		return
	}
	fmt.Println(desc)
}

func (c *client) send(action *flatbuffers.Builder, fbData []byte, desc string) {
	c.sendRaw(kSetBlockAction, fbData, desc)
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

// machineOpen builds a ContainerOpenReq and sends it as MachineOpen (18) or
// MachineClose (46) — same payload, different msg type.
func (c *client) machineOpen(x, y, z int32, playerID uint64, close bool) {
	b := flatbuffers.NewBuilder(64)
	Protocol.ContainerOpenReqStart(b)
	Protocol.ContainerOpenReqAddPlayerId(b, playerID)
	Protocol.ContainerOpenReqAddPos(b, Protocol.CreateVec3i(b, x, y, z))
	req := Protocol.ContainerOpenReqEnd(b)
	b.Finish(req)
	kind := "open  "
	msgType := byte(kMachineOpenReq)
	if close {
		kind = "close "
		msgType = kMachineCloseReq
	}
	c.sendRaw(msgType, b.FinishedBytes(),
		fmt.Sprintf("%s (%d,%d,%d) player=%d", kind, x, y, z, playerID))
}

// slotReq sends SetMachineSlotReq (kSetMachineSlot): move item player↔machine slot.
// player_slot=255: item_id=0 → extract slot to cursor; item_id>0 → put into machine slot.
func (c *client) slotReq(x, y, z int32, playerID uint64, slotIndex uint16, item uint16, count uint8, meta uint16, playerSlot uint8) {
	b := flatbuffers.NewBuilder(64)
	Protocol.SetMachineSlotReqStart(b)
	Protocol.SetMachineSlotReqAddPlayerId(b, playerID)
	Protocol.SetMachineSlotReqAddPos(b, Protocol.CreateVec3i(b, x, y, z))
	Protocol.SetMachineSlotReqAddSlotIndex(b, slotIndex)
	Protocol.SetMachineSlotReqAddItemId(b, item)
	Protocol.SetMachineSlotReqAddCount(b, count)
	Protocol.SetMachineSlotReqAddMeta(b, meta)
	Protocol.SetMachineSlotReqAddPlayerSlot(b, playerSlot)
	req := Protocol.SetMachineSlotReqEnd(b)
	b.Finish(req)
	c.sendRaw(kSetMachineSlot, b.FinishedBytes(),
		fmt.Sprintf("slot  (%d,%d,%d) slot=%d item=0x%04X x%d meta=%d player_slot=%d player=%d",
			x, y, z, slotIndex, item, count, meta, playerSlot, playerID))
}

func (c *client) pipeQuery(x, y, z int32, playerID uint64) {
	b := flatbuffers.NewBuilder(64)
	Protocol.PipeContentsReqStart(b)
	Protocol.PipeContentsReqAddPlayerId(b, playerID)
	Protocol.PipeContentsReqAddPos(b, Protocol.CreateVec3i(b, x, y, z))
	req := Protocol.PipeContentsReqEnd(b)
	b.Finish(req)
	c.sendRaw(kPipeContentsReq, b.FinishedBytes(),
		fmt.Sprintf("pipeq (%d,%d,%d) player=%d", x, y, z, playerID))
}

func formatSlots(u *Protocol.BlockEntityUpdate, input bool) string {
	n := u.InputItemsLength()
	get := u.InputItems
	if !input {
		n = u.OutputItemsLength()
		get = u.OutputItems
	}
	parts := make([]string, 0, n)
	var it Protocol.ItemStack
	for i := 0; i < n; i++ {
		if get(&it, i) {
			parts = append(parts, fmt.Sprintf("[%d]=0x%04X x%d meta=%d", i, it.ItemId(), it.Count(), it.Meta()))
		}
	}
	return strings.Join(parts, " ")
}

func statusName(s Protocol.BlockAckStatus) string {	switch s {
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
			switch header[4] {
			case kBlockAck:
				ack := Protocol.GetRootAsBlockAck(payload, 0)
				var pos Protocol.Vec3i
				ack.Pos(&pos)
				fmt.Printf("  ACK pos=(%d,%d,%d) status=%s (req=%d)\n",
					pos.X(), pos.Y(), pos.Z(), statusName(ack.Status()), ack.RequestId())
			case kBlockEntityUpdate:
				u := Protocol.GetRootAsBlockEntityUpdate(payload, 0)
				var pos Protocol.Vec3i
				u.Pos(&pos)
				if c.watchPos && (pos.X() != c.watchX || pos.Y() != c.watchY || pos.Z() != c.watchZ) {
					continue
				}
				fmt.Printf("  MACHINE pos=(%d,%d,%d) type=0x%04X progress=%.2f energy=%d/%d type=%d\n    in:  %s\n    out: %s\n",
					pos.X(), pos.Y(), pos.Z(), u.MachineType(), u.Progress(),
					u.Energy(), u.EnergyCapacity(), u.EnergyType(),
					formatSlots(u, true), formatSlots(u, false))
			case kSetMachineSlotResp:
				r := Protocol.GetRootAsSetMachineSlotResp(payload, 0)
				var pos Protocol.Vec3i
				r.Pos(&pos)
				fmt.Printf("  SLOTACK pos=(%d,%d,%d) slot=%d success=%v err=%q\n",
					pos.X(), pos.Y(), pos.Z(), r.SlotIdx(), r.Success(), r.Error())
			case kPipeContentsResp:
				r := Protocol.GetRootAsPipeContentsResp(payload, 0)
				var pos Protocol.Vec3i
				r.Pos(&pos)
				if !r.Found() {
					fmt.Printf("  PIPE pos=(%d,%d,%d) NOT FOUND\n", pos.X(), pos.Y(), pos.Z())
					continue
				}
				fmt.Printf("  PIPE pos=(%d,%d,%d) node=%d fluid=0x%X amount=%d/%d\n",
					pos.X(), pos.Y(), pos.Z(), r.NodeId(), r.FluidId(), r.Amount(), r.Capacity())
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
	playerID := flag.Uint64("player", 0, "player id for open/pipe queries")
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
				itemTok := fields[4]
				for i := 4; i+1 < len(fields); i++ {
					if fields[i] == "--expected" {
						itemTok = fields[i+1]
					}
				}
				if fields[0] == "break" {
					item, err := parseItem(itemTok)
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
			case "slot":
				if len(fields) < 7 {
					fmt.Println("bad script line:", line)
					continue
				}
				x, _ := strconv.ParseInt(fields[1], 10, 32)
				y, _ := strconv.ParseInt(fields[2], 10, 32)
				z, _ := strconv.ParseInt(fields[3], 10, 32)
				si, _ := strconv.ParseUint(fields[4], 10, 16)
				item, err := parseItem(fields[5])
				if err != nil {
					fmt.Println("bad item:", err)
					continue
				}
				cnt, _ := strconv.ParseUint(fields[6], 10, 8)
				c.slotReq(int32(x), int32(y), int32(z), *playerID, uint16(si), item, uint8(cnt), 0, 255)
				waitAck(*timeout)
			case "open":
				if len(fields) < 4 {
					fmt.Println("bad script line:", line)
					continue
				}
				x, _ := strconv.ParseInt(fields[1], 10, 32)
				y, _ := strconv.ParseInt(fields[2], 10, 32)
				z, _ := strconv.ParseInt(fields[3], 10, 32)
				c.watchPos = true
				c.watchX, c.watchY, c.watchZ = int32(x), int32(y), int32(z)
				c.machineOpen(int32(x), int32(y), int32(z), *playerID, false)
				waitAck(*timeout)
				c.machineOpen(int32(x), int32(y), int32(z), *playerID, true)
				waitAck(300)
			case "pipe":
				if len(fields) < 4 {
					fmt.Println("bad script line:", line)
					continue
				}
				x, _ := strconv.ParseInt(fields[1], 10, 32)
				y, _ := strconv.ParseInt(fields[2], 10, 32)
				z, _ := strconv.ParseInt(fields[3], 10, 32)
				c.pipeQuery(int32(x), int32(y), int32(z), *playerID)
				waitAck(*timeout)
			default:
				fmt.Println("unknown script cmd:", fields[0])
			}
		}

	case "slot":
		if len(args) < 7 {
			fmt.Println("usage: gateway_cli slot <x> <y> <z> <slot_index> <item_id> <count> [--player N] [--wait ms]")
			os.Exit(2)
		}
		x, _ := strconv.ParseInt(args[1], 10, 32)
		y, _ := strconv.ParseInt(args[2], 10, 32)
		z, _ := strconv.ParseInt(args[3], 10, 32)
		si, _ := strconv.ParseUint(args[4], 10, 16)
		item, err := parseItem(args[5])
		if err != nil {
			fmt.Println(err)
			os.Exit(2)
		}
		cnt, _ := strconv.ParseUint(args[6], 10, 8)
		wait := *timeout
		pid := *playerID
		for i := 7; i < len(args); i++ {
			switch args[i] {
			case "--player":
				if i+1 < len(args) {
					v, err := strconv.ParseUint(args[i+1], 10, 64)
					if err != nil {
						fmt.Println("bad --player:", err)
						os.Exit(2)
					}
					pid = v
					i++
				}
			case "--wait":
				if i+1 < len(args) {
					v, err := strconv.Atoi(args[i+1])
					if err != nil {
						fmt.Println("bad --wait:", err)
						os.Exit(2)
					}
					wait = v
					i++
				}
			}
		}
		c.slotReq(int32(x), int32(y), int32(z), pid, uint16(si), item, uint8(cnt), 0, 255)
		waitAck(wait)

	case "pipe":
		if len(args) < 4 {
			fmt.Println("usage: gateway_cli pipe <x> <y> <z> [--player N] [--wait ms]")
			os.Exit(2)
		}
		x, _ := strconv.ParseInt(args[1], 10, 32)
		y, _ := strconv.ParseInt(args[2], 10, 32)
		z, _ := strconv.ParseInt(args[3], 10, 32)
		wait := *timeout
		pid := *playerID
		for i := 4; i < len(args); i++ {
			switch args[i] {
			case "--player":
				if i+1 < len(args) {
					v, err := strconv.ParseUint(args[i+1], 10, 64)
					if err != nil {
						fmt.Println("bad --player:", err)
						os.Exit(2)
					}
					pid = v
					i++
				}
			case "--wait":
				if i+1 < len(args) {
					v, err := strconv.Atoi(args[i+1])
					if err != nil {
						fmt.Println("bad --wait:", err)
						os.Exit(2)
					}
					wait = v
					i++
				}
			}
		}
		c.pipeQuery(int32(x), int32(y), int32(z), pid)
		waitAck(wait)

	case "open":
		if len(args) < 4 {
			fmt.Println("usage: gateway_cli open <x> <y> <z> [--player N] [--wait ms]")
			os.Exit(2)
		}
		x, _ := strconv.ParseInt(args[1], 10, 32)
		y, _ := strconv.ParseInt(args[2], 10, 32)
		z, _ := strconv.ParseInt(args[3], 10, 32)
		wait := 2000
		pid := *playerID
		for i := 4; i < len(args); i++ {
			switch args[i] {
			case "--player":
				if i+1 < len(args) {
					v, err := strconv.ParseUint(args[i+1], 10, 64)
					if err != nil {
						fmt.Println("bad --player:", err)
						os.Exit(2)
					}
					pid = v
					i++
				}
			case "--wait":
				if i+1 < len(args) {
					v, err := strconv.Atoi(args[i+1])
					if err != nil {
						fmt.Println("bad --wait:", err)
						os.Exit(2)
					}
					wait = v
					i++
				}
			}
		}
		c.watchPos = true
		c.watchX, c.watchY, c.watchZ = int32(x), int32(y), int32(z)
		c.machineOpen(int32(x), int32(y), int32(z), pid, false)
		waitAck(wait)
		c.machineOpen(int32(x), int32(y), int32(z), pid, true)
		waitAck(300)

	default:
		fmt.Println("unknown command:", args[0])
		os.Exit(2)
	}
	fmt.Println("done")
}
