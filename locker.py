class LockedValue:
	
	def __init__(self, value=None):
		self._value = value
		self._locked = True

	@property
	def value(self):
		return self._value

	@value.setter
	def value(self, new):
		if self._locked:
			return
		self._value = new
		self._locked = True

	def unlock(self):
		self._locked = False
