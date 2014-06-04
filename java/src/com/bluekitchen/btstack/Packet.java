package com.bluekitchen.btstack;

public class Packet {
	
	public static final int HCI_COMMAND_PACKET = 1;
	public static final int HCI_EVENT_PACKET   = 4;
	
	protected byte[] data;
	protected int payloadLen;
	protected int packetType;
	protected int channel;
	
	public int getPacketType() {
		return packetType;
	}

	public byte[] getBuffer() {
		return data;
	}
	
	public int getPayloadLen(){
		return payloadLen;
	}

	public int getChannel() {
		return channel;
	}

	public Packet(int packetType, int channel, byte[] buffer, int payloadLen){
		this.packetType = packetType;
		this.channel = channel;
		this.data = new byte[payloadLen];
		System.arraycopy(buffer, 0, this.data, 0, payloadLen);
		this.data = buffer;
		this.payloadLen = payloadLen;
	}

	public String toString(){
		StringBuffer t = new StringBuffer();
		t.append(String.format("Packet %d, channel %d, len %d: ", packetType, channel, payloadLen));
		t.append(Util.asHexdump(data, payloadLen));
		return t.toString();
	}

	public void dump(){
		System.out.println(this.toString());
	}
}
